//
// Created on 11/20/17.
//

#ifndef RPIPE_ECHOBTMPIPE_H
#define RPIPE_ECHOBTMPIPE_H


#include <vector>
#include "../../IPipe.h"

class EchoBtmPipe : public IPipe {
public:
    int Init() override;

    int Send(ssize_t nread, const rbuf_t *buf) override;

    int Close() override;

    void Flush(IUINT32 curr) override;

private:
    std::vector<rbuf_t *> mPendingData;
};


#endif //RPIPE_ECHOBTMPIPE_H
