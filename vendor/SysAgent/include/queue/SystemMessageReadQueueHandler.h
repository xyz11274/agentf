//
// Created by z00913804 on 25-6-16.
//

#ifndef SYSAGENT_SYSTEMMESSAGEREADQUEUEHANDLER_H
#define SYSAGENT_SYSTEMMESSAGEREADQUEUEHANDLER_H

#include "QueueEntity.h"
#include "BlockingMessageQueue.h"

class SystemMessageReadQueueHandler {
public:
    void start();
    void postData(const QueueMessage message);
private:
    BlockingMessageQueue<QueueMessage> msgQueue;
    static PerSession* recoverSessionFromAccessCtrl(const std::shared_ptr<SYSAGENT::AccessCtrl>& data);
};
#endif //SYSAGENT_SYSTEMMESSAGEREADQUEUEHANDLER_H
