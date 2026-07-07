/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#ifndef SYSAGENT_UDSINTERFACE_H
#define SYSAGENT_UDSINTERFACE_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>


namespace SysAgent::UDS {

// ==================== 错误码定义 ====================

    /**
    * @brief UDS 接口错误码
    *
    * 符合 UDS_INTERFACE.md 2.3 错误码定义
    */
    enum class UDSErrorCode : uint32_t {
        SUCCESS = 0,                  // 成功
        ERROR = 1,                    // 通用错误
        TIMEOUT = 2,                  // 请求超时
        INVALID_PARAMS = 3,           // 参数无效
        PLUGIN_NOT_FOUND = 4,         // 插件未找到
        METHOD_NOT_FOUND = 5,         // 方法未找到
        MM_AT_REQUEST_ERR = 1001,     // 请求AgentTool失败
        MM_AT_RESPONSE_ERR = 1002,    // AgentTool响应错误
        MM_AT_RESPONSE_BODY_ERR = 1003, // AgentTool响应体解析失败
        EMBEDDING_TIMEOUT = 0x1000001, // Embedding超时
        EMBEDDING_FAIL = 0x1000002    // Embedding失败
    };

// ==================== 基础数据类型 ====================

    /**
    * @brief 模态类型
    */
    enum class ModalType {
        TEXT = 0,
        AUDIO = 1,
        IMAGE = 2
    };

    /**
    * @brief Embedding 模式
    */
    enum class EmbeddingPattern {
        LOCAL,
        REMOTE
    };

    /**
    * @brief 填充类型
    */
    enum class PaddingType {
        NONE,
        AUTO,
        MAX_LENGTH
    };

    /**
    * @brief 重排模式
    */
    enum class ReRankPattern {
        CROSSED_ENCODER,
        BI_ENCODER,
        HYBRID
    };

    /**
    * @brief XPU 类型
    */
    enum class XpuType {
        CPU,
        GPU,
        NPU
    };

    /**
    * @brief 后端类型
    */
    enum class BackEndType {
        MINDSPORE,
        MINDSPORECLOUD,
        TORCH,
        ONNX
    };

// ==================== 请求/响应结构体 ====================

    /**
    * @brief UDS 请求消息
    *
    * 符合 UDS_INTERFACE.md 2.2 请求消息格式
    */
    struct UDSRequest {
        std::string request_id;       // 请求ID
        std::string plugin_type;      // "LLM", "RAG", "Tool"
        std::string plugin_name;      // 插件名称
        std::string method;           // 方法名
        std::string params_json;      // JSON 格式的参数
        int64_t timestamp;            // 时间戳
    };

    /**
    * @brief UDS 响应消息
    *
    * 符合 UDS_INTERFACE.md 2.2 响应消息格式
    */
    struct UDSResponse {
        std::string request_id;       // 请求ID
        std::string status;           // "success", "error"
        UDSErrorCode error_code = UDSErrorCode::SUCCESS;
        std::string error_message;    // 错误信息
        std::string data_json;        // JSON 格式的数据
        int64_t timestamp;            // 时间戳

        std::string ToJson() const;
    };

    /**
    * @brief UDS 事件消息
    *
    * 符合 UDS_INTERFACE.md 2.2 事件消息格式
    */
    struct UDSEvent {
        std::string event_id;         // 事件ID
        std::string event_type;       // "stream", "notification" 或具体事件类型
        std::string channel;          // 事件频道
        std::string request_id;       // 关联请求ID（客户端 recv 校验配对）
        std::string data_json;        // JSON 格式的数据
        bool done = false;            // 是否完成
        int64_t timestamp;            // 时间戳

        std::string ToJson() const;
    };

// ==================== LLM 相关结构体 ====================

    /**
    * @brief LLM 推理参数
    *
    * 符合 UDS_INTERFACE.md 附录 A.1 MMReasonParam
    */
    struct LLMReasonParam {
        std::string model_service_name;   // 模型服务名称
        std::string model_instance_id;    // 模型实例ID (已废弃)
        std::string model_name;           // 模型名称
        double temperature = 0.7;         // 温度参数
        double top_p = 0.9;               // Top-P 参数
        std::string lora_id;              // 微调模型ID (可选)
        uint32_t context_max_length = 8192; // 上下文最大长度
        uint32_t sched_group = 0;         // 调度组标识
        bool use_third = false;           // 是否使用第三方模型

        // Builder 模式
        class Builder {
        public:
            Builder() = default;

            Builder& WithModelServiceName(const std::string& name) {
                model_service_name_ = name;
                return *this;
            }

            Builder& WithModelName(const std::string& name) {
                model_name_ = name;
                return *this;
            }

            Builder& WithTemperature(double temp) {
                temperature_ = temp;
                return *this;
            }

            Builder& WithTopP(double p) {
                top_p_ = p;
                return *this;
            }

            Builder& WithContextMaxLength(uint32_t len) {
                context_max_length_ = len;
                return *this;
            }

            Builder& WithLoraId(const std::string& id) {
                lora_id_ = id;
                return *this;
            }

            Builder& WithUseThird(bool use) {
                use_third_ = use;
                return *this;
            }

            LLMReasonParam Build() const {
                return LLMReasonParam{
                    model_service_name_, model_instance_id_, model_name_,
                    temperature_, top_p_, lora_id_, context_max_length_,
                    sched_group_, use_third_
                };
            }

        private:
            std::string model_service_name_;
            std::string model_instance_id_;
            std::string model_name_;
            double temperature_ = 0.7;
            double top_p_ = 0.9;
            std::string lora_id_;
            uint32_t context_max_length_ = 8192;
            uint32_t sched_group_ = 0;
            bool use_third_ = false;
        };
    };

/**
* @brief 用户意图
*/
    struct UserIntention {
        std::string query;
        ModalType modal_type = ModalType::TEXT;
    };

/**
* @brief Prompt 键
*/
    struct PromptKey {
        std::string name;
        std::string owner;
        std::string language = "zh";
        std::string version;
        std::string namespace_ = "common";
    };

/**
* @brief Prompt 项
*/
    struct PromptItem {
        PromptKey key;
        std::string content;
    };

/**
* @brief 工具函数参数
*/
    struct ToolFunctionParameters {
        std::string parameters_type = "object";
        std::string properties_json;       // JSON格式的属性定义
        std::vector<std::string> required_list;
        bool additional_properties = false;
    };

/**
* @brief 工具函数定义
*/
    struct ToolFunction {
        std::string function_name;
        bool strict = false;
        std::string function_description;
        ToolFunctionParameters parameters;
    };

/**
* @brief 工具定义
*/
    struct ToolDefinition {
        std::string tools_type = "function";
        ToolFunction tools_function;
    };

/**
* @brief LLM 请求
*
* 符合 UDS_INTERFACE.md 附录 A.2 MMLLMRequest
*/
    struct LLMRequest {
        std::vector<UserIntention> intentions;
        std::vector<PromptItem> prompt;
        std::vector<ToolDefinition> tools;
    };

/**
* @brief LLM 响应
*/
    struct LLMResponse {
        std::string response;
        std::string model;
        uint32_t prompt_tokens = 0;
        uint32_t completion_tokens = 0;
        uint32_t total_tokens = 0;
        std::string finish_reason;
    };

/**
* @brief LLM 流式任务信息
*/
    struct LLMTaskInfo {
        std::string task_id;
        bool created = false;
    };

// ==================== Tool 相关结构体 ====================

/**
* @brief 工具信息
*/
    struct ToolInfo {
        std::string name;
        std::string description;
        std::string input_schema_json;   // JSON Schema for input
        std::string output_schema_json;  // JSON Schema for output
    };

/**
* @brief 工具调用结果内容
*/
    struct ToolContent {
        std::string type;      // "text"
        std::string text_json; // JSON 格式的文本内容
    };

/**
* @brief 工具调用结果
*/
    struct ToolCallResult {
        std::vector<ToolContent> content;
        UDSErrorCode error_code = UDSErrorCode::SUCCESS;
    };

// ==================== RAG 相关结构体 ====================

// ==================== 插件信息结构体 ====================

/**
* @brief 日志配置
*/
    struct LogConfig {
        std::string model_id;
        int level_debug = 1;
        int level_info = 1;
        int level_warn = 1;
        int level_error = 1;
        int level_fatal = 1;
    };

/**
* @brief 插件信息
*/
    struct PluginInfo {
        std::string name;
        std::string type;
        std::vector<std::string> tags;
        std::string version;
        std::string description;
        std::vector<std::string> capabilities;
    };

// ==================== 心跳响应 ====================

/**
* @brief 心跳响应
*/
    struct HeartbeatResponse {
        bool pong = false;
        int64_t server_time = 0;
    };

// ==================== 订阅信息 ====================

/**
* @brief 订阅响应
*/
    struct SubscriptionInfo {
        bool subscribed = false;
        std::string subscription_id;
    };

// ==================== 回调类型定义 ====================

/**
* @brief 响应回调函数类型
*/
    using ResponseCallback = std::function<void(const UDSResponse&)>;

/**
* @brief 事件回调函数类型
*/
    using EventCallback = std::function<void(const UDSEvent&)>;

/**
* @brief LLM 流式回调接口
*
* 符合 UDS_INTERFACE.md 4.2.2 事件推送格式
*/
    class LLMStreamCallback {
    public:
        virtual ~LLMStreamCallback() = default;

        /**
         * @brief LLM 流式开始
         * @param task_id 任务ID
         * @param request_id 请求ID
         */
        virtual void OnLLMStart(const std::string& task_id, const std::string& request_id) = 0;

        /**
         * @brief LLM 流式结束
         * @param task_id 任务ID
         * @param request_id 请求ID
         */
        virtual void OnLLMEnd(const std::string& task_id, const std::string& request_id) = 0;

        /**
         * @brief LLM 新 Token
         * @param task_id 任务ID
         * @param token Token 内容
         */
        virtual void OnLLMNewToken(const std::string& task_id, const std::string& token) = 0;

        /**
         * @brief LLM 最终答案
         * @param task_id 任务ID
         * @param answer 最终答案
         */
        virtual void OnLLMFinalAnswer(const std::string& task_id, const std::string& answer) = 0;

        /**
         * @brief LLM 错误
         * @param task_id 任务ID
         * @param error_code 错误码
         * @param error_message 错误信息
         */
        virtual void OnLLMError(const std::string& task_id, UDSErrorCode error_code,
            const std::string& error_message) = 0;
    };

// ==================== UDS 客户端接口 ====================

/**
* @brief UDS 客户端接口
*
* 提供与插件通信的统一接口
* 符合 UDS_INTERFACE.md 第3章通用接口
*/
    class UDSClient {
    public:
        virtual ~UDSClient() = default;

        // ==================== 连接管理 ====================

        /**
         * @brief 连接到 UDS 服务端
         * @param socket_path Socket 路径，默认 /var/run/agentflow/plugin.sock
         * @return 是否连接成功
         */
        virtual bool Connect(const std::string& socket_path) = 0;

        /**
         * @brief 断开连接
         */
        virtual void Disconnect() = 0;

        /**
         * @brief 检查是否已连接
         */
        virtual bool IsConnected() const = 0;

        /**
         * @brief 发送心跳
         * @return 心跳响应
         */
        virtual HeartbeatResponse Heartbeat() = 0;

        // ==================== 通用请求 ====================

        /**
         * @brief 发送请求并等待响应
         * @param request 请求消息
         * @param timeout_ms 超时时间（毫秒）
         * @return 响应消息
         */
        virtual UDSResponse SendRequest(const UDSRequest& request, int timeout_ms = 30000) = 0;

        /**
         * @brief 异步发送请求
         * @param request 请求消息
         * @param callback 响应回调
         */
        virtual void SendRequestAsync(const UDSRequest& request, ResponseCallback callback) = 0;

        // ==================== 事件订阅 ====================

        /**
         * @brief 订阅事件
         * @param channels 事件频道列表
         * @param callback 事件回调
         * @return 订阅信息
         */
        virtual SubscriptionInfo Subscribe(const std::vector<std::string>& channels,
            EventCallback callback) = 0;

        /**
         * @brief 取消订阅
         * @param subscription_id 订阅ID
         * @return 是否成功
         */
        virtual bool Unsubscribe(const std::string& subscription_id) = 0;

        // ==================== 插件管理 ====================

        /**
         * @brief 获取插件列表
         * @param plugin_type 插件类型（可选）"LLM", "RAG", "Tool"
         */
        virtual std::vector<PluginInfo> ListPlugins(const std::string& plugin_type = "") = 0;

        /**
         * @brief 获取插件信息
         * @param plugin_name 插件名称
         */
        virtual PluginInfo GetPluginInfo(const std::string& plugin_name) = 0;

        /**
         * @brief 初始化插件
         * @param plugin_name 插件名称
         * @param config_path 配置路径
         * @param log_config 日志配置
         */
        virtual bool InitPlugin(const std::string& plugin_name,
            const std::string& config_path = "",
            const LogConfig& log_config = LogConfig{}) = 0;
    };

// ==================== LLM UDS 接口 ====================

/**
* @brief LLM UDS 接口
*
* 符合 UDS_INTERFACE.md 第4章 LLMPlugin 接口
*/
    class LLMUDSInterface {
    public:
        virtual ~LLMUDSInterface() = default;

        // ==================== 非流式推理 ====================

        /**
         * @brief 非流式推理
         * @param plugin_name 插件名称
         * @param param 推理参数
         * @param request 请求内容
         * @param timeout_ms 超时时间
         * @return 推理结果
         */
        virtual LLMResponse Complete(
            const std::string& plugin_name,
            const LLMReasonParam& param,
            const LLMRequest& request,
            int timeout_ms = 60000) = 0;

        // ==================== 流式推理 ====================

        /**
         * @brief 创建流式任务
         * @param plugin_name 插件名称
         * @param param 推理参数
         * @param callback_channel 回调频道（可选）
         * @return 任务信息
         */
        virtual LLMTaskInfo CreateTask(
            const std::string& plugin_name,
            const LLMReasonParam& param,
            const std::string& callback_channel = "") = 0;

        /**
         * @brief 流式推理
         * @param plugin_name 插件名称
         * @param task_id 任务ID
         * @param request 请求内容
         * @param callback 流式回调
         * @return 是否成功启动
         */
        virtual bool StreamComplete(
            const std::string& plugin_name,
            const std::string& task_id,
            const LLMRequest& request,
            std::shared_ptr<LLMStreamCallback> callback) = 0;

        /**
         * @brief 销毁任务
         * @param plugin_name 插件名称
         * @param task_id 任务ID
         * @return 是否成功
         */
        virtual bool DestroyTask(
            const std::string& plugin_name,
            const std::string& task_id) = 0;

        // ==================== 上下文管理 ====================

        /**
         * @brief 中断流式请求
         * @param plugin_name 插件名称
         * @param task_id 任务ID
         * @return 是否成功
         */
        virtual bool InterruptStream(
            const std::string& plugin_name,
            const std::string& task_id) = 0;

        /**
         * @brief 获取上下文
         * @param plugin_name 插件名称
         * @param task_id 任务ID
         * @return 上下文内容（JSON格式）
         */
        virtual std::string GetContext(
            const std::string& plugin_name,
            const std::string& task_id) = 0;

        /**
         * @brief 更新上下文
         * @param plugin_name 插件名称
         * @param task_id 任务ID
         * @param context_json 上下文JSON
         * @return 是否成功
         */
        virtual bool UpdateContext(
            const std::string& plugin_name,
            const std::string& task_id,
            const std::string& context_json) = 0;

        /**
         * @brief 追加上下文
         * @param plugin_name 插件名称
         * @param task_id 任务ID
         * @param context_json 上下文JSON
         * @return 是否成功
         */
        virtual bool AddContext(
            const std::string& plugin_name,
            const std::string& task_id,
            const std::string& context_json) = 0;
    };

// ==================== Tool UDS 接口 ====================

/**
* @brief Tool UDS 接口
*
* 符合 UDS_INTERFACE.md 第5章 ToolPlugin 接口
*/
    class ToolUDSInterface {
    public:
        virtual ~ToolUDSInterface() = default;

        /**
         * @brief 列出工具
         * @param plugin_name 插件名称
         * @return 工具列表
         */
        virtual std::vector<ToolInfo> ListTools(const std::string& plugin_name) = 0;

        /**
         * @brief 调用工具
         * @param plugin_name 插件名称
         * @param tool_name 工具名称
         * @param params_json 参数JSON
         * @return 调用结果
         */
        virtual ToolCallResult CallTool(
            const std::string& plugin_name,
            const std::string& tool_name,
            const std::string& params_json) = 0;
    };

// ==================== 工厂函数 ====================

/**
* @brief 创建 UDS 客户端
* @return UDS 客户端实例
*/
    std::unique_ptr<UDSClient> CreateUDSClient();

/**
* @brief 创建 LLM UDS 接口
* @param client UDS 客户端
* @return LLM UDS 接口实例
*/
    std::unique_ptr<LLMUDSInterface> CreateLLMUDSInterface(UDSClient* client);

    /**
    * @brief 创建 Tool UDS 接口
    * @param client UDS 客户端
    * @return Tool UDS 接口实例
    */
    std::unique_ptr<ToolUDSInterface> CreateToolUDSInterface(UDSClient* client);


// ==================== 辅助函数 ====================

    /**
    * @brief 获取默认 UDS Socket 路径
    * @return 默认路径
    */
    std::string GetDefaultSocketPath();

    /**
    * @brief 错误码转字符串
    * @param code 错误码
    * @return 错误描述
    */
    const char* UDSErrorCodeToString(UDSErrorCode code);

} // namespace SysAgent::uds
#endif // SYSAGENT_UDSINTERFACE_H
