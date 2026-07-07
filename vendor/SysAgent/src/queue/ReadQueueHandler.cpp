//
// Created by y00601565 on 2025/2/24.
//

#include <cstring>

#include "message/ServerEntity.h"
#include "queue/QueueManager.h"
#include "queue/WebSocketReadQueueHandler.h"
#include "TeamEnvironment.h"
#include "TEManager.h"
#include "TeHelper.h"

std::string findTaskId(std::shared_ptr<SYSAGENT::AccessCtrl> data)
{
    std::string teamEnvId = "";
    switch (data->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT: {
            teamEnvId = data->taskAssignment.header.taskID;
            break;
        }
        case SYSAGENT::EventType::TASKSTOP: {
            teamEnvId = data->taskStop.header.taskID;
            break;
        }
        case SYSAGENT::EventType::USERINPUT: {
            teamEnvId = data->userInput.header.taskID;
            break;
        }
        case SYSAGENT::EventType::USEROUTPUT: {
            teamEnvId = data->userOutput.header.taskID;
            break;
        }
        case SYSAGENT::EventType::DATAEXCHANGE: {
            teamEnvId = data->dataExchange.header.taskID;
            break;
        }
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT: {
            teamEnvId = data->taskStatusManagement.header.taskID;
            break;
        }
        case SYSAGENT::EventType::LOGINOUT: {
            teamEnvId = data->loginOut.header.taskID;
            break;
        }
        case SYSAGENT::EventType::LOCATION: {
            teamEnvId = data->locationInfo.header.taskID;
            break;
        }
        default:
            break;
    }
    return teamEnvId;
}

void WebSocketReadQueueHandler::start()
{
    LOG_INFO("WebSocketReadQueueHandler started processing message queue...");
    while (true) {
        try {
            LOG_INFO_EX("WebSocketReadQueueHandler processing message queue");
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
            std::shared_ptr<SYSAGENT::AccessCtrl> data = msg.data;
            LOG_INFO("Successfully read message, preparing to process...");
            if (!data) {
                LOG_WARN("Message data pointer is null after assignment");
                continue;
            }
            LOG_INFO_EX("get data message: %s", data->toString().c_str());

            // 判断是否lifecycle消息
            if (data->eventType == SYSAGENT::EventType::LOGINOUT) {
                TEHelper::CleanTE(data, msg.session);
                continue;
            }
            // 判断消息类型，如果是系统消息类型，放到系统消息队里里面，不阻塞原有的业务报文处理
            if (data->eventType == SYSAGENT::EventType::LOCATION
            || data->eventType == SYSAGENT::EventType::USERPORTRAITCONF) {
                QueueManager::getInstance().postSystemMessageReadQueue(data, msg.session);
                continue;
            }
            // 使用TEHelper获取或创建TeamEnvironment
            std::shared_ptr<TeamEnvironment> access = TEHelper::GetOrCreateTE(data, msg.session);
            // 创建失败的话，打印错误日志
            if (!access) {
                LOG_ERR_EX("Failed to find or create valid TeamEnvironment");
                // Add error recovery logic here
                continue;
            }

            access->session = msg.session;
            access->updateUserSession(data->header.clientSessionId, data->header.acmSessionId);
            LOG_INFO_EX("Starting intent processing, forwarding to intent manager");
            TEHelper::RunGraph(data);
        } catch (const std::exception& e) {
            LOG_ERR_EX("Exception occurred while processing message: %s", e.what());
        }
    }
}

void WebSocketReadQueueHandler::postData(const QueueMessage message) { msgQueue.PostMessage(message); }

// 从 AccessCtrl 中恢复 Session 的辅助函数
PerSession* WebSocketReadQueueHandler::recoverSessionFromAccessCtrl(const std::shared_ptr<SYSAGENT::AccessCtrl>& data)
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
