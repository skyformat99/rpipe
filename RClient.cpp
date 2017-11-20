//
// Created on 11/15/17.
//

#include <uv.h>
#include <cstdlib>
#include <nmq.h>
#include <sstream>
#include "RClient.h"
#include "TopStreamPipe.h"
#include "NMQPipe.h"

RClient::~RClient() {
}

int RClient::Loop(Config &conf) {
    mLoop = uv_default_loop();
    int nret = uv_ip4_addr(conf.param.targetIp, conf.param.targetPort, &mTargetAddr);
    if (nret != 0) {
        fprintf(stderr, "wrong conf: %s\n", uv_strerror(nret));
        return -1;
    }

    mListenHandle = InitTcpListen(conf);
    if (!mListenHandle) {
        fprintf(stderr, "failed to listen tcp\n");
        return 1;
    }

    mBrigde = CreateBridgePipe(conf);

    uv_timer_init(mLoop, &mFlushTimer);

    mFlushTimer.data = this;
    uv_timer_start(&mFlushTimer, flush_cb, conf.param.interval * 2, conf.param.interval);

    uv_run(mLoop, UV_RUN_DEFAULT);

    Close();
    return 0;
}

IPipe *RClient::CreateBtmPipe(const Config &conf) {
    uv_udp_t *dgram = static_cast<uv_udp_t *>(malloc(sizeof(uv_udp_t)));
    uv_udp_init(mLoop, dgram);

    return new BtmDGramPipe(dgram);
}

void RClient::close_cb(uv_handle_t *handle) {
    free(handle);
}

void RClient::Close() {
    if (mBrigde) {
        mBrigde->SetHashRawDataFunc(nullptr);
        mBrigde->SetOnFreshDataCb(nullptr);
        mBrigde->SetOnErrCb(nullptr);

        mBrigde->Close();
        delete mBrigde;
        mBrigde = nullptr;
    }

    if (mListenHandle) {
        uv_close(mListenHandle, close_cb);
        mListenHandle = nullptr;
    }
}

uv_handle_t *RClient::InitTcpListen(const Config &conf) {
    uv_tcp_t *tcp = static_cast<uv_tcp_t *>(malloc(sizeof(uv_tcp_t)));
    tcp->data = this;
    uv_tcp_init(mLoop, tcp);

    struct sockaddr_in addr = {0};
    uv_ip4_addr(conf.param.localListenIface, conf.param.localListenPort, &addr);
    int nret = uv_tcp_bind(tcp, reinterpret_cast<const sockaddr *>(&addr), 0);
    if (0 == nret) {
        nret = uv_listen(reinterpret_cast<uv_stream_t *>(tcp), conf.param.BACKLOG, svr_conn_cb);
        if (nret) {
            fprintf(stderr, "failed to listen tcp: %s\n", uv_strerror(nret));
            uv_close(reinterpret_cast<uv_handle_t *>(tcp), close_cb);
        }
    } else {
        fprintf(stderr, "failed to bind %s:%d, error: %s\n", conf.param.localListenIface, conf.param.localListenPort,
                uv_strerror(nret));
        free(tcp);
    }

    if (nret) {
        fprintf(stderr, "failed to start: %s\n", uv_strerror(nret));
        tcp = nullptr;
    }

    return reinterpret_cast<uv_handle_t *>(tcp);
}

void RClient::svr_conn_cb(uv_stream_t *server, int status) {
    RClient *rc = static_cast<RClient *>(server->data);
    rc->newConn(server, status);
}

void RClient::newConn(uv_stream_t *server, int status) {
    if (status) {
        fprintf(stderr, "new connection error. status: %d, err: %s\n", status, uv_strerror(status));
        return;
    }

    uv_tcp_t *client = static_cast<uv_tcp_t *>(malloc(sizeof(uv_tcp_t)));
    uv_stream_t *stream = reinterpret_cast<uv_stream_t *>(client);
    uv_tcp_init(mLoop, client);
    if (uv_accept(server, stream) == 0) {
        onNewClient(stream);
    } else {
        uv_close(reinterpret_cast<uv_handle_t *>(client), close_cb);
    }
}

void RClient::onNewClient(uv_stream_t *client) {
    mConv++;
    IPipe *top = new TopStreamPipe(client);
    IPipe *nmq = new NMQPipe(mConv, top);   // nmq pipe addr
    nmq->SetTargetAddr(&mTargetAddr);

    char buf[16] = {0};
    sprintf(buf, "%u", mConv);
    rbuf_t rbuf = {0};
    rbuf.base = buf;
    ssize_t len = strlen(buf);
    auto key = HashKeyFunc(len, &rbuf);

    mBrigde->AddPipe(key, nmq);
}

BridgePipe *RClient::CreateBridgePipe(const Config &conf) {
    IPipe *btmPipe = CreateBtmPipe(conf);
    if (!btmPipe) {
        fprintf(stderr, "failed to start.\n");
        return nullptr;
    }

    auto *pipe = new BridgePipe(btmPipe);
    pipe->SetOnFreshDataCb(nullptr);    // explicitly set cb. ignore unknown data

    pipe->SetOnErrCb([this](IPipe *pipe, int err) {
//        uv_timer_stop(&this->mFlushTimer);
        fprintf(stderr, "bridge pipe error: %d. Exit!\n", err);
        uv_stop(this->mLoop);
    });
    pipe->SetHashRawDataFunc(HashKeyFunc);
    pipe->Init();
    return pipe;
}

void RClient::Flush() {
    mBrigde->Flush(iclock());
}

BridgePipe::KeyType RClient::HashKeyFunc(ssize_t nread, const rbuf_t *buf) {
    if (nread >= sizeof(IUINT32) && buf->base) {
        return  std::to_string(nmq_get_conv(buf->base));
    }
    return "";
}

