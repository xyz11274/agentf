//
// Created by z00913804 on 25-8-14.
//

#ifndef SYSAGENT_KMMMSGREADQUEUEYHANDLER_H
#define SYSAGENT_KMMMSGREADQUEUEYHANDLER_H
#include "QueueEntity.h"
#include "BlockingMessageQueue.h"
#include "memory/History.h"
class KmmMsgReadQueueHandler {
public:
    void start();
    void postData(const Memory& memory);
private:
    BlockingMessageQueue<Memory> msgQueue;
    void addKmmMemoryBatchHandler(std::vector<Memory>& msgs);
};
#endif // SYSAGENT_KMMMSGREADQUEUEYHANDLER_H
