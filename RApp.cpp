//
// Created on 11/15/17.
//

#include <nmq/util.h>
#include "uv.h"
#include "RApp.h"
#include "util/FdUtil.h"
#include "plog/Log.h"
#include "plog/Appenders/ConsoleAppender.h"
#include "util/ProcUtil.h"
#include "nmq/NMQPipe.h"
#include "util/UvUtil.h"

RApp::~RApp() {
    if (mConsoleAppender) {
        delete mConsoleAppender;
        mConsoleAppender = nullptr;
    }
    if (mFileAppender) {
        delete mFileAppender;
        mFileAppender = nullptr;
    }
}

int RApp::Init() {
    if (!mConf.Inited()) {
        fprintf(stderr, "configuration not inited.\n");
        return -1;
    }
    return doInit();
}

void RApp::Close() {
    if (mExitSig) {
        uv_signal_stop(mExitSig);
        uv_close(reinterpret_cast<uv_handle_t *>(mExitSig), close_cb);
        mExitSig = nullptr;
    }

    if (mBridge) {
        mBridge->Close();
        delete mBridge;
        mBridge = nullptr;
    }

    if (mFlushTimer) {
        uv_timer_stop(mFlushTimer);
        uv_close(reinterpret_cast<uv_handle_t *>(mFlushTimer), close_cb);
        mFlushTimer = nullptr;
    }

    if (mLoop) {
        uv_stop(mLoop);
        if (mConf.isDaemon) {   // it will crash if delete default loop
            if (!uv_loop_close(mLoop)) {
                free(mLoop);
            } else {
                LOGE << "loop not closed properly";
            }
        }
        mLoop = nullptr;
    }
}

int RApp::initLog(const Config &conf) {
    if (!conf.log_path.empty()) {
        if (!FdUtil::FileExists(conf.log_path.c_str())) {
            int nret = FdUtil::CreateFile(conf.log_path);
            if (nret < 0) {
                fprintf(stderr, "failed to create log file: %s", strerror(errno));
                return nret;
            }
        }
        mFileAppender = new plog::RollingFileAppender<plog::TxtFormatter>(conf.log_path.c_str(), 100000, 5);
    } else {
        fprintf(stderr, "warning: log path empty\n");
    }

    mConsoleAppender = new plog::ConsoleAppender<plog::TxtFormatter>();
    plog::init(conf.log_level, mConsoleAppender);
    if (mFileAppender) {
        plog::get()->addAppender(mFileAppender);
    }

    return 0;
}

int RApp::Parse(int argc, char **argv) {
    return mConf.Parse(isServer(), argc, argv);
}

int RApp::doInit() {
    assert(mConf.Inited());
    mLoop = uv_default_loop();
    int nret = initLog(mConf);
    if (nret) {
        return nret;
    }
    LOGV << "conf: " << mConf.to_json().dump();
    nret = uv_ip4_addr(mConf.param.targetIp.c_str(), mConf.param.targetPort, &mTargetAddr);
    if (nret) {
        return nret;
    }

    if (makeDaemon()) {
        return -1;
    }

    initMTU();

    IPipe *btmPipe = CreateBtmPipe(mConf, mLoop);
    if (!btmPipe) {
        return -1;
    }
    mBridge = CreateBridgePipe(mConf, btmPipe, mLoop);
    if (!mBridge || mBridge->Init()) {
        return -1;
    }

    mFlushTimer = static_cast<uv_timer_t *>(malloc(sizeof(uv_timer_t)));
    uv_timer_init(mLoop, mFlushTimer);
    mFlushTimer->data = this;

    watchExitSignal();
    return 0;
}

int RApp::makeDaemon() {
    int n = ProcUtil::MakeDaemon(mConf.isDaemon); // todo: add daemon test later
    if (n < 0) {
        LOGE << "make process daemon failed: " << strerror(errno);
        return n;
    }
    // todo: if run in daemon, poll will fail if use default loop (on mac, it's uv__io_check_fd fails). why?
    if (mConf.isDaemon) {
        LOGI << "Run in background. pid: " << getpid(); // print to file.
        mLoop = static_cast<uv_loop_t *>(malloc(sizeof(uv_loop_t)));
        memset(mLoop, 0, sizeof(uv_loop_t));
        uv_loop_init(mLoop);
    }
    return 0;
}

int RApp::Start() {
    uv_timer_start(mFlushTimer, flush_cb, mConf.param.interval, mConf.param.interval);
    return uv_run(mLoop, UV_RUN_DEFAULT);
}

void RApp::Flush() {
    mBridge->Flush(iclock());
}

const Config &RApp::GetConfig() {
    return mConf;
}

uv_loop_t *RApp::GetLoop() {
    return mLoop;
}

void RApp::flush_cb(uv_timer_t *handle) {
    RApp *app = static_cast<RApp *>(handle->data);
    app->Flush();
}


void RApp::close_signal_handler(uv_signal_t *handle, int signum) {
    RApp *app = static_cast<RApp *>(handle->data);
    app->onExitSignal();
}

void RApp::onExitSignal() {
    LOGD << "Receive exit signal. Exit!";
    Close();
}

void RApp::watchExitSignal() {
    if (!mExitSig) {
        mExitSig = UvUtil::WatchSignal(mLoop, SIG_EXIT, close_signal_handler, this);
        assert(mExitSig);
    }
}

BridgePipe *RApp::GetBridgePipe() {
    return mBridge;
}

const sockaddr_in *RApp::GetTarget() {
    return &mTargetAddr;
}

INMQPipe *RApp::NewNMQPipeFromConf(uint32_t conv, const Config &conf, IPipe *top) {
    auto nmq = new NMQPipe(conv, top);
    nmq->SetNmqMTU(conf.param.mtu);
    nmq->SetWndSize(conf.param.sndwnd, conf.param.rcvwnd);
    nmq->SetFlowControl(conf.param.fc);
    nmq->SetInterval(conf.param.interval);
    nmq->SetAckLimit(conf.param.dupAckLim);
    nmq->SetTolerance(conf.param.tolerance);

    return nmq;
}

void RApp::initMTU() {
    assert(mConf.param.mtu > ISessionPipe::HEAD_LEN);
    mNmqMtu = mConf.param.mtu - ISessionPipe::HEAD_LEN; // attention, all header should be calculated
}

uint32_t RApp::GET_NMQ_MTU() const {
    return mNmqMtu;
}
