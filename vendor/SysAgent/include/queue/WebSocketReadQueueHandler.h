#pragma once

#include "queue/BlockingMessageQueue.h"
#include "queue/QueueEntity.h"

class WebSocketReadQueueHandler {
public:
    void start();
    void postData(const QueueMessage message);

private:
    BlockingMessageQueue<QueueMessage> msgQueue;
    static PerSession* recoverSessionFromAccessCtrl(const std::shared_ptr<SYSAGENT::AccessCtrl>& data);
};
