/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#ifndef SYSAGENT_UDSPLUGINSERVER_H
#define SYSAGENT_UDSPLUGINSERVER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "UDSInterface.h"
#include "IMInferServiceClient.h"
#include "llm_plugin/llmplugin.h"
#include "tool_plugin/tool_plugin.h"
#include "context_plugin/context_plugin.h"

namespace SysAgent {

// Forward declarations
    namespace plugin {
        class LLMPlugin;
        class ToolPlugin;
        class RAGPlugin;
        class Plugin;
    }  // namespace plugin

    /**
    * @brief UDS 插件服务端配置
    */
    struct UDSPluginServerConfig {
        std::string socket_path = "/var/run/agentflow/plugin.sock";  // Socket 路径
        int max_connections = 10;           // 最大连接数
        int recv_buffer_size = 4096;        // 接收缓冲区大小
        int send_buffer_size = 4096;        // 发送缓冲区大小
        int request_timeout_ms = 60000;     // 请求超时时间
        bool enable_streaming = true;       // 是否启用流式支持
        int heartbeat_interval_ms = 30000;  // 心跳间隔
    };

    /**
    * @brief 客户端连接信息
    */
    struct ClientConnection {
        int fd = -1;                        // Socket 文件描述符
        std::string client_id;              // 客户端 ID
        bool active = false;                // 是否活跃
        std::vector<std::string> subscriptions;  // 订阅列表
    };

    /**
     * RAG查询结果
     */
    struct RAGResult {
        bool similar;
        std::string category;
        std::string intent;
        std::string data;
        std::map<std::string, std::string> params;

        std::string ToJson();
    };

    /**
    * @brief UDS 插件服务端
    *
    * 接收客户端通过 UDS 发送的请求，路由到相应的插件处理。
    * 支持 LLM、RAG、Tool 三种插件类型。
    *
    * 协议说明：
    *   LLM 仅支持 llm.chat（新协议，单步 OpenAI 兼容调用），
    *   旧三段式协议（llm.task.create / llm.stream / llm.task.destroy / llm.complete /
    *   llm.interrupt / llm.context.*）已全部移除。
    */
    class UDSPluginServer {
    public:
        /**
         * @brief 构造函数
         * @param config 服务端配置
         */
        explicit UDSPluginServer(const UDSPluginServerConfig& config = UDSPluginServerConfig{});

        /**
         * @brief 析构函数
         */
        ~UDSPluginServer();

        /**
         * @brief 启动服务端
         * @return 是否成功启动
         */
        bool Start();

        /**
         * @brief 停止服务端
         */
        void Stop();

        /**
         * @brief 检查是否运行中
         * @return 是否运行中
         */
        bool IsRunning() const { return running_.load(); }

        /**
         * @brief 获取配置
         * @return 配置引用
         */
        const UDSPluginServerConfig& GetConfig() const { return config_; }

        /**
         * @brief 注册 LLM 插件
         * @param name 插件名称
         * @param plugin 插件实例
         */
        void RegisterLLMPlugin(const std::string& name, std::shared_ptr<AgentFrm::Plugin::LLMPlugin> plugin);

        /**
         * @brief 注册 Tool 插件
         * @param name 插件名称
         * @param plugin 插件实例
         */
        void RegisterToolPlugin(const std::string& name, std::shared_ptr<AgentFrm::Plugin::ToolPlugin> plugin);

        /**
        * @brief 注册 上下文 插件
        * @param name 插件名称
        * @param plugin 插件实例
        */
        void RegisterContextPlugin(const std::string& name, ContextPlugin::ContextPlugin& plugin);

        /**
         * @brief 注销插件
         * @param name 插件名称
         */
        void UnregisterPlugin(const std::string& name);

    private:
        // 主服务循环
        void ServerLoop();

        // 帮助函数，发送所有数据
        bool SendAll(int fd, const char* buffer, size_t length);

        // 处理客户端连接
        void HandleClient(int client_fd);

        // 接收消息（4字节长度前缀 + 消息体）
        bool RecvMessage(int fd, std::string& message);

        // 发送消息（4字节长度前缀 + 消息体）
        bool SendMessage(int fd, const std::string& message);

        // 发送响应
        void SendResponse(int fd, const UDS::UDSResponse& response);

        // 发送事件
        void SendEvent(int fd, const UDS::UDSEvent& event);

        // 处理请求
        void HandleRequest(int client_fd, const std::string& request_json);

        // ========== 插件管理接口处理 ==========

        // 处理 plugin.list
        std::string HandlePluginList(const std::string& params_json);

        // 处理 plugin.info
        std::string HandlePluginInfo(const std::string& params_json);

        // 处理 plugin.init
        std::string HandlePluginInit(const std::string& params_json);

        // ========== LLM 接口处理 ==========

        // 处理 llm.chat（单步 OpenAI 兼容调用，新协议唯一 LLM 方法）
        std::string HandleLLMChat(const std::string& params_json, int client_fd);

        // ========== Tool 接口处理 ==========

        // 处理 tool.list
        std::string HandleToolList(const std::string& params_json);

        // 处理 tool.call
        std::string HandleToolCall(const std::string& params_json);

        // ========== RAG 接口处理 ==========

        // 处理 rag.retrieve.search
        std::string HandleRAGRetrieveSearch(const std::string& params_json);

        // ========== 上下文接口处理 ==========

        std::string HandleContextServiceInvoke(const std::string& method, const std::string& params_json);

        // ========== SKILL接口处理 ==========

        std::string HandleSkillProxyMessageInvoke(const std::string& request_json);

        // ========== 消息直出接口处理 ==========

        std::string HandleDirectMessageInvoke(const std::string& params_json);

        // ========== 消息直出接口处理 ==========

        std::string HandleLocalMessageSendTest(const std::string& params_json);

        // ========== 事件订阅处理 ==========

        // 处理 event.subscribe
        std::string HandleEventSubscribe(const std::string& params_json, int client_fd);

        // 处理 event.unsubscribe
        std::string HandleEventUnsubscribe(const std::string& params_json);

        // ========== 心跳处理 ==========

        // 处理 heartbeat
        std::string HandleHeartbeat();

        // ========== 辅助方法 ==========

        // 清理 socket 文件
        void CleanupSocket();

        // 生成唯一 ID
        std::string GenerateId(const std::string& prefix);

        // 获取当前时间戳
        int64_t GetCurrentTimestamp();

        // 构建成功响应
        std::string BuildSuccessResponse(const std::string& request_id, const std::string& data_json);

        // 构建错误响应
        std::string BuildErrorResponse(const std::string& request_id, UDS::UDSErrorCode code,
            const std::string& message);

        // 构建事件消息
        std::string BuildEventMessage(const UDS::UDSEvent& event);

    private:
        UDSPluginServerConfig config_;
        std::atomic<bool> running_{false};
        int server_fd_{-1};

        std::thread server_thread_;
        std::mutex clients_mutex_;
        std::map<int, ClientConnection> clients_;

        // 插件存储
        std::mutex plugins_mutex_;
        std::map<std::string, std::shared_ptr<AgentFrm::Plugin::LLMPlugin>> llm_plugins_;
        std::map<std::string, std::shared_ptr<AgentFrm::Plugin::ToolPlugin>> tool_plugins_;
        std::map<std::string, ContextPlugin::ContextPlugin*> context_plugins_;

        // RAG 资源管理
        std::mutex rag_resources_mutex_;
        std::map<std::string, std::shared_ptr<void>> embeddings_;
        std::map<std::string, std::shared_ptr<void>> datasources_;
        std::map<std::string, std::shared_ptr<void>> rerankers_;
        std::map<std::string, std::shared_ptr<void>> retrievers_;

        // 事件订阅
        std::mutex subscriptions_mutex_;
        std::map<std::string, std::vector<int>> channel_subscribers_;  // channel -> client_fds

        // ID 生成计数器
        std::atomic<uint64_t> id_counter_{0};
    };
}  // namespace agentflow

#endif //SYSAGENT_UDSPLUGINSERVER_H
