//
// Created by y00601565 on 2025/2/25.
//

#include "queue/QueueManager.h"
#include <thread>

void QueueManager::startWriteQueueHandlers() {
    writeHandler.start();
}

void QueueManager::startReadQueueHandlers() {
    readHandler.start();
}

void QueueManager::startSystemMessageReadQueueHandler() {
    systemMessageReadQueueHandler.start();
}

void QueueManager::startKmmMsgReadQueueHandler()
{
    kmmMsgReadQueueHandler.start();
}

void QueueManager::startEventMarkMsgReadQueueHandler()
{
    eventMarkMsgReadQueueHandler.start();
}
void QueueManager::init() {
    std::thread readThread([this]() {
        QueueManager::startReadQueueHandlers();
    });
    readThread.detach();
    std::thread writeThread([this]() {
        QueueManager::startWriteQueueHandlers();
    });
    writeThread.detach();
    std::thread systemReadThread([this]() {
        QueueManager::startSystemMessageReadQueueHandler();
    });
    systemReadThread.detach();
    // KmmMsgReadQueueHandler 内部调用 SendAsync（场景2风险），新流程不使用，关闭
    // std::thread kmmReadThread([this]() {
    //     QueueManager::startKmmMsgReadQueueHandler();
    // });
    // kmmReadThread.detach();
    std::thread eventMarkReadThread([this]() {
        QueueManager::startEventMarkMsgReadQueueHandler();
    });
    eventMarkReadThread.detach();
    LOG_INFO("init queueManager success.. ");
}
void QueueManager::postReadQueue(const std::shared_ptr<SYSAGENT::AccessCtrl> &data, PerSession *session) {
    QueueMessage message = QueueMessage {
        data,
        session
    };
    readHandler.postData(message);
}

void QueueManager::postWriteQueue(const std::shared_ptr<SYSAGENT::AccessCtrl> &data, PerSession *session) {
    QueueMessage message = QueueMessage {
        data,
        session
    };
    writeHandler.writeData(message);
}

void QueueManager::postSystemMessageReadQueue(const std::shared_ptr<SYSAGENT::AccessCtrl> &data, PerSession *session) {
    QueueMessage message = QueueMessage {
        data,
        session
    };
    systemMessageReadQueueHandler.postData(message);
}

// 执行任务时满足条件调用该方法传入报文，再组装Memory
void QueueManager::postKmmMsgReadQueue(const Memory& memory)
{
    kmmMsgReadQueueHandler.postData(memory);
}
std::vector<std::string> getIndexArray(const std::string& taskID)
{
    std::vector<std::string> result;
    size_t start = 0, end = 0;
    while ((end = taskID.find('_', start)) != std::string::npos) {
        // 将子字符串转换为整数并添加到结果数组中
        result.push_back(taskID.substr(start, end - start));
        start = end + 1;
    }
    // 处理最后一个子字符串
    result.push_back(taskID.substr(start));
    return result;
}
void QueueManager::postEventMarkMsgReadQueue(const EventMarkMsg &eventMarkMsg)
{
    EventMarkingMsg eventMarkingMsg;
    eventMarkingMsg.timeStamp = eventMarkMsg.timeStamp;
    eventMarkingMsg.logLevel = eventMarkMsg.logLevel;
    eventMarkingMsg.eventType = eventMarkMsg.eventType;
    eventMarkingMsg.userID = eventMarkMsg.accessCtrl->header.userName;
    auto accessCtrl = eventMarkMsg.accessCtrl;
    if (accessCtrl == nullptr) {
        LOG_INFO_EX("postEventMarkMsg to Queue failed, accessCtrl is nullptr");
        return;
    }
    std::vector<std::string> indexs = getIndexArray(accessCtrl->header.taskID);
    std::string workflowID;
    if (!indexs.empty()) {
        workflowID = indexs[0];
    }
    eventMarkingMsg.taskIdr = "-" + workflowID +"-" + accessCtrl->header.taskID;
    eventMarkingMsg.msg.serviceName = "SysAgent";
    eventMarkingMsg.msg.eventName = "SysAgent." + eventMarkMsg.eventName; // SysAgent.ModuleName.EventName
    eventMarkingMsg.msg.status = eventMarkMsg.status;
    eventMarkingMsg.msg.details = eventMarkMsg.details;
    eventMarkMsgReadQueueHandler.postData(eventMarkingMsg);
}