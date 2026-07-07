//
// Created by z00913804 on 25-6-16.
//
#include "rag/datasource/data_source.h"

#include "memory/Location.h"
#include "queue/QueueManager.h"
#include "queue/SystemMessageReadQueueHandler.h"
#include "queue/Lifecycle/LifecycleHandler.h"
#include "TeHelper.h"

void SystemMessageReadQueueHandler::start() {
    LOG_INFO( "SystemMessageReadQueueHandler::start processing system message queue");
    while (true) {
        try {
            LOG_INFO_EX("SystemMessageReadQueueHandler processing system message queue");
            // 等待接收消息
            QueueMessage msg = msgQueue.GetMessage();
            // 验证消息内容
            if (!msg.data) {
                LOG_WARN("Received invalid message, data is null");
                continue;
            }
            // 检查session是否为空
            if (!msg.session) {
                LOG_WARN("Session pointer is null, attempting to retrieve from AccessCtrl data");
                // 调用辅助函数从 AccessCtrl 恢复 Session
                msg.session = recoverSessionFromAccessCtrl(msg.data);
                if (!msg.session) {
                    LOG_ERR("Failed to recover session from AccessCtrl data");
                    continue;
                }
            }
            // 读取消息
            std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = msg.data;
            LOG_INFO("Successfully read message, preparing to process...");
            if (!accessCtrl) {
                LOG_WARN("Message data pointer is null after assignment");
                continue;
            }
            LOG_INFO_EX("get data message: %s", accessCtrl->toString().c_str());

            auto lifecycleList = LifecycleHandlerManager::GetInstance().getHandlerList();
            if (lifecycleList.find(accessCtrl->eventType) != lifecycleList.end()) {
                auto lifecycleHandler = lifecycleList.at(accessCtrl->eventType);
                lifecycleHandler->handle(accessCtrl);
            }
        } catch (const std::exception& e) {
            LOG_ERR_EX("Exception occurred while processing message: %s", e.what());
        }
    }

}

void SystemMessageReadQueueHandler::postData(const QueueMessage message)
{
    msgQueue.PostMessage(message);
}

// 从 AccessCtrl 中恢复 Session 的辅助函数
PerSession* SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl(const std::shared_ptr<SYSAGENT::AccessCtrl>& data)
{
    if (!data) {
        LOG_ERR("Cannot recover session: data pointer is invalid");
        return nullptr;
    }
    try {
        // 使用新创建的函数从 AccessCtrl 报文体中解析 TEid
        std::string teId = TEHelper::extractTEidFromMessage(data);
        if (teId.empty()) {
            LOG_ERR("Failed to extract TEid from AccessCtrl message");
            return nullptr;
        }
        LOG_INFO_EX("Extracted TEid: %s from AccessCtrl", teId.c_str());
        // 通过 TEid 获取对应的 TeamEnvironment
        std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(teId);
        if (!te) {
            LOG_ERR_EX("Failed to find TeamEnvironment with id: %s", teId.c_str());
            return nullptr;
        }
        // 使用新创建的 getSession 方法获取 session
        PerSession* session = te->getSession();
        if (!session) {
            LOG_ERR("Failed to get session from TeamEnvironment");
            return nullptr;
        }
        LOG_INFO_EX("Successfully recovered session from TeamEnvironment with id: %s", teId.c_str());
        return session;
    } catch (const std::exception& e) {
        LOG_ERR_EX("Exception while attempting to recover session: %s", e.what());
        return nullptr;
    }
}