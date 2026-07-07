#pragma once

#include "queue/BlockingMessageQueue.h"
#include "queue/QueueEntity.h"
#include "graphManager/GraphUtils.h"
class WebSocketWriteQueueHandler {
public:
    void start();
    void writeData(const QueueMessage message);
private:
    BlockingMessageQueue<QueueMessage> msgQueue;
};


