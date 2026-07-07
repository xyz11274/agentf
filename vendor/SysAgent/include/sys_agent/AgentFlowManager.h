/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#ifndef SYSAGENT_AGENTADAPTOR_H
#define SYSAGENT_AGENTADAPTOR_H

#include <memory>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>
#include <map>
#include "framework_common/utils/class_util.h"
#include "framework_common/threadpool/threadpool.h"
#include "agent/agent.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"
#include "sys_agent/UDSPluginServer.h"

#include "aisf_platform/parser/aisf_msg_ctrl.h"

using namespace agentflow;
using namespace SYSAGENT;

namespace SysAgent {

    /**
     * Agent消息回调
     */
    using MessageCallback = std::function<void(const std::shared_ptr<AccessCtrl>& accessCtrl, const std::string&,
        const std::string&)>;

    /**
     * 单个Agent初始化配置
     */
    struct AgentInitConfig {
        std::string agentName;
        std::string agentPackagePath;
        MessageCallback messageCallback;
    };

    /**
     * agentflow manager初始化配置
     */
    struct AgentFlowInitConfig {
        std::vector<AgentInitConfig> agents;
    };

    struct AgentInfo {
        AgentInitConfig agentInitConfig;
        agentflow::runtime::MMDataBus* dataBus;
        std::string agentId;
        std::shared_ptr<agentflow::framework::MMAgent> agent;

        AgentInfo(AgentInitConfig agentInitConfig,
            const std::shared_ptr<agentflow::framework::MMAgent>& agent,
            const std::unique_ptr<agentflow::runtime::MMAgentRuntime>& runtime) {
            this->agentInitConfig = std::move(agentInitConfig);
            this->agent = agent;
            dataBus = &runtime->get_databus(agent->get_id());
            agentId = agent->get_id();
        }
    };

    struct AgentFlowMsg {
        std::string dialogueId;
        std::string sessionId;
        std::shared_ptr<AccessCtrl> msg;

        std::string ToJson() const;

        bool FromJson(const std::string& json_str);
    };

    /**
     * AgentAdaptor
     * 1. 通过AgentFlow拉起Agent
     * 2. 发送消息到指定Agent
     * 3. 接收消息时推送到
     */
    class AgentFlowManager : CMFrm::Utils::SingletonBase {
    public:
        /**
         * 获取单例
         * @return
         */
        static std::shared_ptr<AgentFlowManager> GetAgentFlowManager();

        /**
         * 初始化方法
         * @return
         */
        bool Init(const AgentFlowInitConfig& agentFlowInitConfig);

        /**
         * 发送消息
         * @return
         */
        bool SendMsg(const std::shared_ptr<AccessCtrl>& accessCtrl,
            const std::string& dialogueId = "",
            const std::string& agentflowTaskId = "",
            const std::string& agentName = "system_agent_v3");

        /**
         * 终止单个任务
         * @param accessCtrl
         * @param agentName
         */
        void StopTask(const std::shared_ptr<AccessCtrl>& accessCtrl, const std::string& agentName =
        "sys_agent");


        static AgentFlowMsg ConvertMMAgentMsgToAccessCtrl(const framework::MMAgentMsgQue::Message& message);

        ~AgentFlowManager() override;
    private:
        /**
         * 通过AgentName找到AgentInfo
         * @param agentName
         * @return agentInfo
         */
        std::shared_ptr<AgentInfo> FindAgentInfoByAgentName(const std::string& agentName);

        /**
         * 开始监听数据总线中的消息
         */
        void StartListeningDataBus();

        /**
         * 取消监听
         */
        void StopListening();

        /**
         * 初始化UDS插件Server
         * @return
         */
        bool InitUDSPluginServer();

        AgentFlowManager() = default;

        // 单例参数
        static std::shared_ptr<AgentFlowManager> agentFlowManagerInstance;
        static std::once_flag agentFlowManagerOnce;

        // agent相关参数
        std::map<std::string, std::shared_ptr<AgentInfo>> agentMap;
        std::unique_ptr<agentflow::runtime::MMAgentRuntime> runtime;

        // 监听线程相关参数
        std::atomic<bool> isStartListening{false};
        std::thread listeningThread;
        CMFrm::ThreadPool::ThreadPool agentThreadPool{5, "AgentFlowManager"};

        // 插件相关参数
        std::shared_ptr<UDSPluginServer> udsPluginServer;
    };
}

#endif //SYSAGENT_AGENTADAPTOR_H
