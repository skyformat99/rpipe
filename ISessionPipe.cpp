//
// Created on 12/8/17.
//

#include "ISessionPipe.h"
#include <sstream>
#include <cassert>
#include <cstring>

#include <nmq/enc.h>

ISessionPipe::ISessionPipe(IPipe *topPipe, const KeyType &key, const sockaddr_in *target) : ITopContainerPipe(topPipe) {
    if (target) {
        mAddr = static_cast<sockaddr_in *>(malloc(sizeof(struct sockaddr_in)));
        memcpy(mAddr, target, sizeof(struct sockaddr_in));
    }
    mKey = key;
    mConv = decodeConv(key);
    assert(mKey == BuildKey(mConv, target));    // ensure consistency
    assert(mConv > 0);
}

ISessionPipe::~ISessionPipe() {
    assert(mAddr == nullptr);
}

ISessionPipe::KeyType ISessionPipe::GetKey() {
    return mKey;
}

int ISessionPipe::Close() {
    ITopContainerPipe::Close();
    if (mAddr) {
        free(mAddr);
        mAddr = nullptr;
    }
    return 0;
}

int ISessionPipe::IsCloseSignal(ssize_t nread, const rbuf_t *buf) {
    if (nread >= HEAD_LEN && buf && buf->base) {
        char cmd;
        uint32_t conv;
        decodeHead(buf->base, nread, &cmd, &conv);
        return cmd == RST || cmd == FIN;
    }
    return 0;
}

ssize_t ISessionPipe::insertHead(char *base, int len, char cmd, uint32_t conv) {
    assert(base != nullptr);
    assert(len >= HEAD_LEN);

    base[0] = cmd;
    encode_uint32(conv, base + 1);
    return sizeof(conv) + 1;
}

const char *ISessionPipe::decodeHead(const char *base, int len, char *cmd, uint32_t *conv) {
    assert(base != nullptr);
    assert(len >= HEAD_LEN);
    *cmd = base[0];
    return decode_uint32(conv, base + 1);
}

ISessionPipe::KeyType ISessionPipe::BuildKey(uint32_t conv, const struct sockaddr_in *addr) {
    std::ostringstream out;
    if (addr == nullptr) {
        out << ":" << std::to_string(conv);
    } else {
        out << inet_ntoa(addr->sin_addr) << ":" << ntohs(addr->sin_port) << ":" << std::to_string(conv);
    }
    return out.str();
}

ISessionPipe::KeyType ISessionPipe::BuildKey(ssize_t nread, const rbuf_t *rbuf) {
    if (nread >= HEAD_LEN && rbuf && rbuf->base) {
        char cmd;
        uint32_t conv;
        decodeHead(rbuf->base, nread, &cmd, &conv);

        struct sockaddr_in *addr = static_cast<sockaddr_in *>(rbuf->data);
        return BuildKey(conv, addr);
    }
    return nullptr;
//#ifndef NNDEBUG
//    return nullptr;
//#else
//    return "";
//#endif
}

uint32_t ISessionPipe::ConvFromKey(const ISessionPipe::KeyType &key) {
    if (!key.empty()) {
        ssize_t pos = key.rfind(':');
        if (pos != std::string::npos) {
            return stoul(key.substr(pos + 1));
        }
    }
    return 0;
}

uint32_t ISessionPipe::decodeConv(const ISessionPipe::KeyType &key) {
    auto pos = key.rfind(':');
    if (pos == std::string::npos || pos == key.length() - 1) {
        return 0;
    }
    unsigned long conv = std::stoul(key.substr(pos + 1));
    return static_cast<uint32_t>(conv);
}

void ISessionPipe::notifyPeerClose(char cmd) {
    rbuf_t rbuf = {0};
    char base[HEAD_LEN] = {0};
    ssize_t n = insertHead(base, HEAD_LEN, cmd, mConv);
    rbuf.base = base;
    rbuf.data = mAddr;
    ISessionPipe::Send(n, &rbuf);
}