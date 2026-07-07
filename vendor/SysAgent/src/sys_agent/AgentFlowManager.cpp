/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for Agent.cpp
 */
#include "sys_agent/AgentFlowManager.h"
#include "utils/logger.h"
#include "llm_plugin/llmplugin.h"
#include "rapidjson/rapidjson.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"

using namespace SYSAGENT;
namespace SysAgent {
    std::shared_ptr<AgentFlowManager> AgentFlowManager::agentFlowManagerInstance;
    std::once_flag AgentFlowManager::agentFlowManagerOnce;

    std::string AgentFlowMsg::ToJson() const
    {
        rapidjson::Document doc;
        doc.SetObject();
        rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

        // 字符串字段
        doc.AddMember("dialogueId", rapidjson::Value(dialogueId.c_str(), allocator), allocator);
        doc.AddMember("sessionId", rapidjson::Value(sessionId.c_str(), allocator), allocator);
        doc.AddMember("msg", rapidjson::Value(msg->toString().c_str(), allocator), allocator);

        // 生成最终 JSON 字符串
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        return buffer.GetString();
    }

    bool AgentFlowMsg::FromJson(const std::string& json_str)
    {
        rapidjson::Document doc;
        doc.Parse(json_str.c_str());

        // JSON 格式错误
        if (doc.HasParseError() || !doc.IsObject()) {
            return false;
        }

        // 安全解析每个字段
        if (doc.HasMember("dialogueId") && doc["dialogueId"].IsString())
            dialogueId = doc["dialogueId"].GetString();
        else
            dialogueId.clear();

        if (doc.HasMember("sessionId") && doc["sessionId"].IsString())
            sessionId = doc["sessionId"].GetString();
        else
            sessionId.clear();

        if (doc.HasMember("msg") && doc["msg"].IsString()) {
            msg = std::make_shared<AccessCtrl>();
            std::string accessStr = doc["msg"].GetString();
            if (!msg->Parse(accessStr.c_str(), accessStr.size()).empty()) {
                LOG_ERR("parse msg failed: %s", accessStr.c_str());
                msg = nullptr;
                return false;
            }
        } else {
            msg = std::make_shared<AccessCtrl>();
            if (!msg->Parse(json_str.c_str(), json_str.size()).empty()) {
                LOG_ERR("parse msg failed: %s", json_str.c_str());
                msg = nullptr;
                return false;
            }
        }

        return true;
    }

    std::shared_ptr<AgentFlowManager> AgentFlowManager::GetAgentFlowManager()
    {
        std::call_once(agentFlowManagerOnce, []() {
            agentFlowManagerInstance = std::shared_ptr<AgentFlowManager>(new AgentFlowManager());
        });
        return agentFlowManagerInstance;
    }

    AgentFlowManager::~AgentFlowManager()
    {
        for (const auto& [agentName, agent] : agentMap) {
            if (agent != nullptr) {
                LOG_WARN("Stop agent %s...", agentName.c_str());
                auto ret = agent->agent->stop();
                if (!ret) {
                    LOG_WARN("Stop agent %s failed", agentName.c_str());
                    continue;
                }
                LOG_WARN("Stop agent %s success", agentName.c_str());
            }
        }
        if (!runtime) {
            LOG_WARN("Stop agentflow runtime...");
            auto ret = runtime->stop();
            if (!ret) {
                LOG_WARN("Stop agentflow runtime failed");
            }
        }
        StopListening();
        agentThreadPool.Shutdown();
    }

    bool AgentFlowManager::Init(const AgentFlowInitConfig& agentFlowInitConfig)
    {
        LOG_INFO("start Init agents");
        agentflow::runtime::MMAgentRuntime::Config config;
        config.worker_threads = 100;
        runtime = std::make_unique<agentflow::runtime::MMAgentRuntime>(config);
        if (!runtime || !runtime->start()) {
            LOG_ERR("agentflow runtime Init failed");
            return false;
        }

        for (const auto& agentConfig : agentFlowInitConfig.agents) {
            LOG_INFO("start Init agent: %s, path: %s", agentConfig.agentName.c_str(),
                agentConfig.agentPackagePath.c_str());
            auto agent = agentflow::framework::MMAgent::create_from_package(agentConfig.agentPackagePath);
            if (!agent || !agent->initialize()) {
                LOG_ERR("create agent failed for agent: %s, path %s ", agentConfig.agentName.c_str(),
                    agentConfig.agentPackagePath.c_str());
                return false;
            }
            if (!runtime->register_agent(agent)) {
                LOG_ERR("register to runtime failed for agent: %s, path %s ", agentConfig.agentName.c_str(),
                    agentConfig.agentPackagePath.c_str());
                return false;
            }
            if (!agent->start()) {
                LOG_ERR("register to runtime failed for agent: %s, path %s ", agentConfig.agentName.c_str(),
                    agentConfig.agentPackagePath.c_str());
                return false;
            }
            agentMap[agentConfig.agentName] = std::make_shared<AgentInfo>(agentConfig, agent, runtime);
            LOG_INFO("Init agent: %s, path: %s success", agentConfig.agentName.c_str(),
                agentConfig.agentPackagePath.c_str());
        }
        StartListeningDataBus();
        InitUDSPluginServer();
        return true;
    }

    std::shared_ptr<AgentInfo> AgentFlowManager::FindAgentInfoByAgentName(const std::string& agentName)
    {
        auto iter = agentMap.find(agentName);
        if (iter == agentMap.end()) {
            // 诊断日志：打印 agentMap 状态，帮助定位多线程可见性问题
            std::string keys;
            for (const auto& kv : agentMap) {
                if (!keys.empty()) keys += ", ";
                keys += kv.first;
            }
            LOG_INFO("agentMap.find(%s) FAILED, map_size=%zu keys=[%s]",
                agentName.c_str(), agentMap.size(), keys.c_str());
            return nullptr;
        }
        return iter->second;
    }

    /**
     * 将aisf的accessCtrl消息转换为AgentFlow的message
     * @param accessCtrl
     * @return
     */
    framework::MMAgentMsgQue::Message ConvertAccessCtrlToMMAgentMsg(const std::shared_ptr<AccessCtrl>& accessCtrl)
    {
        framework::MMAgentMsgQue::Message message;
        message.metadata[framework::MMAgentMsgQue::ControlMetadata::USER_ID] = accessCtrl->header.userName;
        message.metadata[framework::MMAgentMsgQue::ControlMetadata::CLIENT_ID] = accessCtrl->header.deviceId;
        message.content = accessCtrl->toString();
        return message;
    }

    /**
     * 将AgentFlow返回的Message转换为accessCtrl
     * @param message
     * @return
     */
    AgentFlowMsg AgentFlowManager::ConvertMMAgentMsgToAccessCtrl(const framework::MMAgentMsgQue::Message& message)
    {
        AgentFlowMsg agentFlowOutputMsg;
        if (!agentFlowOutputMsg.FromJson(message.content)) {
            LOG_ERR("ConvertMMAgentMsgToAccessCtrl parse error");
            return agentFlowOutputMsg;
        }
        return agentFlowOutputMsg;
    }

    bool AgentFlowManager::SendMsg(const std::shared_ptr<AccessCtrl>& accessCtrl,
        const std::string& dialogueId, const std::string& sessionId, const std::string& agentName)
    {
        auto agentInfo = FindAgentInfoByAgentName(agentName);
        if (!agentInfo) {
            LOG_INFO("failed to find agent %s", agentName.c_str());
            return false;
        }

        auto message = ConvertAccessCtrlToMMAgentMsg(accessCtrl);
        LOG_INFO("SendMsg: content_len=%zu did=%s sid=%s agent=%s",
                 message.content.size(), dialogueId.c_str(), sessionId.c_str(), agentName.c_str());

        if (!dialogueId.empty() && !sessionId.empty()) {
            LOG_INFO("SendMsg: REENTRY mode, set dialogue_id=%s session_id=%s task_id=%s_coordinator",
                     dialogueId.c_str(), sessionId.c_str(), sessionId.c_str());
            message.metadata["dialogue_id"] = dialogueId;
            message.metadata["session_id"] = sessionId;
            message.metadata["task_id"] = sessionId + "_coordinator";
        } else {
            LOG_INFO("SendMsg: FIRST mode (empty did/sid), framework generates internal IDs");
        }

        if (!agentInfo->agent->route_request_message(message)) {
            LOG_ERR("[AgentAdaptor] ERROR: Failed to route message %s", message.content.c_str());
            return false;
        }

        LOG_INFO("[AgentAdaptor] Message routed to agent");
        return true;
    }

    void AgentFlowManager::StartListeningDataBus()
    {
        if (isStartListening) {
            return;
        }
        isStartListening = true;
        LOG_INFO("[AgentAdaptor] Listener thread started");
        for (const auto& [agentName, agentConf] : agentMap) {
            agentThreadPool.Submit([this, agentConfig = agentConf]() {
                while (this->isStartListening) {
                    agentConfig->agent->process_all_sessions_until_idle();
                    agentflow::framework::MMAgentMsgQue::Message response;
                    while (agentConfig->agent->dequeue_response_message(response)) {
                        LOG_INFO("[AgentAdaptor] Response received from agent: %s, resp: %s",
                            agentConfig->agentInitConfig.agentName.c_str(), response.content.c_str());
                        AgentFlowMsg agentFlowMsg;
                        if (agentConfig->agentInitConfig.messageCallback) {
                            agentFlowMsg = ConvertMMAgentMsgToAccessCtrl(response);
                            // 优先使用 metadata 中的原始 dialogueId (haDid)，
                            // 避免 response.content 中的内部 ID 导致 TE 查找失败
                            auto did_meta = response.metadata.find("dialogue_id");
                            if (did_meta != response.metadata.end() &&
                                !did_meta->second.empty()) {
                                LOG_INFO("[AgentAdaptor] override dialogueId: content=%s -> metadata=%s",
                                    agentFlowMsg.dialogueId.c_str(), did_meta->second.c_str());
                                agentFlowMsg.dialogueId = did_meta->second;
                            } else {
                                LOG_INFO("[AgentAdaptor] no metadata dialogue_id, using content: %s",
                                    agentFlowMsg.dialogueId.c_str());
                            }
                            LOG_INFO("[AgentAdaptor] callback dialogueId=%s sessionId=%s",
                                agentFlowMsg.dialogueId.c_str(), agentFlowMsg.sessionId.c_str());
                            agentConfig->agentInitConfig.messageCallback(agentFlowMsg.msg,
                                agentFlowMsg.dialogueId, agentFlowMsg.sessionId);
                        }
                        LOG_INFO("clear databus for dialogue %s sessionId %s", agentFlowMsg.dialogueId.c_str(),
                            agentFlowMsg.sessionId.c_str());
                        auto nums = agentConfig->agent->clear_dialogue_data(agentFlowMsg.sessionId, agentFlowMsg
                            .dialogueId);
                        LOG_INFO("clear databus for dialogue %s sessionId %s, nums: %d", agentFlowMsg.dialogueId
                            .c_str(), agentFlowMsg.sessionId.c_str(), nums);
                        if (response.metadata["is_done"] == "true") {
                            LOG_INFO("sys agent get is_done = true");
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        }
    }

    void AgentFlowManager::StopListening()
    {
        if (!isStartListening) {
            return;
        }

        isStartListening = false;
    }

    bool AgentFlowManager::InitUDSPluginServer()
    {
        UDSPluginServerConfig config;
        config.socket_path = "/tmp/agentflow_plugin.sock";
        config.max_connections = 100;
        config.request_timeout_ms = 60000;

        this->udsPluginServer = std::make_shared<UDSPluginServer>(config);

        // 注册插件
        udsPluginServer->RegisterLLMPlugin("LLMPlugin", AgentFrm::Plugin::LLMPlugin::GetInstance());
        udsPluginServer->RegisterToolPlugin("ToolPlugin", AgentFrm::Plugin::ToolPlugin::GetInstance());
        udsPluginServer->RegisterContextPlugin("ContextPlugin", ContextPlugin::ContextPlugin::GetInstance());
        if (!udsPluginServer->Start()) {
            LOG_ERR("Failed to start UDS Plugin Server");
            return false;
        }
        LOG_INFO("Server started successfully!");
        return true;
    }

    void AgentFlowManager::StopTask(const shared_ptr<AccessCtrl>& accessCtrl, const string& agentName)
    {

    }

}
