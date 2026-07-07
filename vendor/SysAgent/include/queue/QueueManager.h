#pragma once

#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "message/ServerEntity.h"
#include "queue/WebSocketReadQueueHandler.h"
#include "queue/WebSocketWriteQueueHandler.h"
#include "queue/SystemMessageReadQueueHandler.h"
#include "queue/KmmMsgReadQueueHandler.h"
#include "queue/EventMarkReadQueueHandler.h"
#include "graphManager/GraphUtils.h"
class QueueManager {
private:
    WebSocketWriteQueueHandler writeHandler{};
    WebSocketReadQueueHandler  readHandler{};
    // 新增SystemReadQueueHandler处理用户定位信息，通过接收到的经纬度信息去MCP的接口获取详细的位置信息，并将相关信息存入数据库
    SystemMessageReadQueueHandler systemMessageReadQueueHandler{};
    // 新增KmmMsgReadQueueHandler批量处理用户原始记忆
    KmmMsgReadQueueHandler kmmMsgReadQueueHandler{};
    // 新增EventMarkMsg队列处理打点消息
    EventMarkMsgReadQueueHandler eventMarkMsgReadQueueHandler{};
    QueueManager() {}  // 私有构造函数
    QueueManager(const QueueManager&) = delete;
    void startReadQueueHandlers();
    void startWriteQueueHandlers();
    void startSystemMessageReadQueueHandler();
    void startKmmMsgReadQueueHandler();
    void startEventMarkMsgReadQueueHandler();
public:
    static QueueManager& getInstance() {
        static QueueManager manager;
        return manager;
    }
    void init();
    void postWriteQueue(const std::shared_ptr<SYSAGENT::AccessCtrl>& data, PerSession *session);
    void postReadQueue(const std::shared_ptr<SYSAGENT::AccessCtrl>& data, PerSession *session);
    void postSystemMessageReadQueue(const std::shared_ptr<SYSAGENT::AccessCtrl>& data, PerSession *session);
    void postKmmMsgReadQueue(const Memory& memory);
    void postEventMarkMsgReadQueue(const EventMarkMsg& eventMarkMsg);
    void EventMark(const std::string &logLevel, const std::string &eventName,
                   const std::string &status, const std::string &details,
                   const std::shared_ptr<SYSAGENT::AccessCtrl> &accessCtrl,
                   const std::string &eventType = "EM") {
        postEventMarkMsgReadQueue(EventMarkMsg(AisfPlat::Utils::GetCurrentTimeMillisecondStr(),
                                               logLevel, details, eventName, status, accessCtrl,
                                               eventType));
    }
};

