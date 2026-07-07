/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */
#include "sys_agent/UDSPluginServer.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include "sys_agent/AgentFlowManager.h"

#include <chrono>
#include <cstring>
#include <future>
#include <sstream>
#include <thread>

#include "skill_plugin.h"
#include "sys_agent/IMInferServiceClient.h"
#include "model/PromptManager.h"
#include "config/Config.h"

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/prettywriter.h"
using rapidjson::Document;
using rapidjson::Value;
using rapidjson::Writer;
using rapidjson::StringBuffer;
using rapidjson::kObjectType;
using rapidjson::kArrayType;

#include "framework_common/constants/number_constant.h"
#include "llm_plugin/llmplugin.h"
#include "tool_plugin/tool_plugin.h"
#include "utils/logger.h"
#include "embedding_option.h"
#include "memory/GaussClient.h"
#include "aisf_platform/parser/json_parser.h"
#include "memory/SafetyFence.h"
#include "memory/ContextManager.h"
#include "callBack/callBack.h"

using namespace DM::RAG;

namespace SysAgent {

// ==================== JSON 辅助函数 ====================

    namespace {
        // JSON 转义
        std::string JsonEscape(const std::string& s) {
            std::string result;
            result.reserve(s.size() + 16);
            for (char c : s) {
                switch (c) {
                    case '"': result += "\\\""; break;
                    case '\\': result += "\\\\"; break;
                    case '\b': result += "\\b"; break;
                    case '\f': result += "\\f"; break;
                    case '\n': result += "\\n"; break;
                    case '\r': result += "\\r"; break;
                    case '\t': result += "\\t"; break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                            result += buf;
                        } else {
                            result += c;
                        }
                }
            }
            return result;
        }

        // 从 RapidJSON Value 获取字符串
        std::string GetString(const Value& v, const char* key, const std::string& default_val = "") {
            if (!v.HasMember(key) || !v[key].IsString()) {
                return default_val;
            }
            return v[key].GetString();
        }

        // 从 RapidJSON Value 获取整数
        int64_t GetInt64(const Value& v, const char* key, int64_t default_val = 0) {
            if (!v.HasMember(key) || !v[key].IsInt64()) {
                return default_val;
            }
            return v[key].GetInt64();
        }

        // 从 RapidJSON Value 获取浮点数
        double GetDouble(const Value& v, const char* key, double default_val = 0.0) {
            if (!v.HasMember(key) || !v[key].IsDouble()) {
                return default_val;
            }
            return v[key].GetDouble();
        }

        // 从 RapidJSON Value 获取布尔值
        bool GetBool(const Value& v, const char* key, bool default_val = false) {
            if (!v.HasMember(key) || !v[key].IsBool()) {
                return default_val;
            }
            return v[key].GetBool();
        }

        // 将 RapidJSON Value 转为字符串
        std::string ValueToString(const Value& v) {
            rapidjson::StringBuffer buffer;
            Writer<rapidjson::StringBuffer> writer(buffer);
            v.Accept(writer);
            return buffer.GetString();
        }

    }  // anonymous namespace

// ==================== UDSPluginServer 实现 ====================

        UDSPluginServer::UDSPluginServer(const UDSPluginServerConfig& config)
            : config_(config) {
        }

        UDSPluginServer::~UDSPluginServer() {
            Stop();
        }

        bool UDSPluginServer::Start() {
            if (running_.load()) {
                return true;
            }

            // 清理可能存在的旧 socket 文件
            CleanupSocket();

            // 创建 socket
            server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
            if (server_fd_ < 0) {
                LOG_ERR("[UDS_PLUGIN_SERVER] Failed to create socket: %s", strerror(errno));
                return false;
            }

            // 设置非阻塞模式
            int flags = fcntl(server_fd_, F_GETFL, 0);
            if (flags < 0 || fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
                LOG_ERR("[UDS_PLUGIN_SERVER] Failed to set non-blocking: %s", strerror(errno));
                close(server_fd_);
                server_fd_ = -1;
                return false;
            }

            // 绑定地址
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, config_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

            if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                LOG_ERR("[UDS_PLUGIN_SERVER] Failed to bind socket %s: %s",
                    config_.socket_path.c_str(), strerror(errno));
                close(server_fd_);
                server_fd_ = -1;
                return false;
            }

            // 监听
            if (listen(server_fd_, config_.max_connections) < 0) {
                LOG_ERR("[UDS_PLUGIN_SERVER] Failed to listen: %s", strerror(errno));
                close(server_fd_);
                server_fd_ = -1;
                CleanupSocket();
                return false;
            }

            running_ = true;
            server_thread_ = std::thread(&UDSPluginServer::ServerLoop, this);

            LOG_INFO("[UDS_PLUGIN_SERVER] Started on %s", config_.socket_path.c_str());
            return true;
        }

        void UDSPluginServer::Stop() {
            if (!running_.exchange(false)) {
                return;
            }

            LOG_INFO("[UDS_PLUGIN_SERVER] Stopping...");

            // 关闭 server socket 以唤醒 accept()
            if (server_fd_ >= 0) {
                close(server_fd_);
                server_fd_ = -1;
            }

            // 等待服务线程结束
            if (server_thread_.joinable()) {
                server_thread_.join();
            }

            // 关闭所有客户端连接
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (auto& [fd, conn] : clients_) {
                    close(fd);
                }
                clients_.clear();
            }

            CleanupSocket();

            LOG_INFO("[UDS_PLUGIN_SERVER] Stopped");
        }

        void UDSPluginServer::ServerLoop() {
            while (running_.load()) {
                // 使用 poll 等待连接
                struct pollfd pfd;
                pfd.fd = server_fd_;
                pfd.events = POLLIN;
                pfd.revents = 0;

                int ret = poll(&pfd, 1, 100);  // 100ms 超时
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (ret == 0) continue;  // 超时，检查 running_

                if (!(pfd.revents & POLLIN)) continue;

                // 接受新连接
                int client_fd = accept(server_fd_, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG_ERR("[UDS_PLUGIN_SERVER] Accept failed: %s", strerror(errno));
                    }
                    continue;
                }

                // 设置非阻塞
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                // 添加到客户端列表
                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    ClientConnection conn;
                    conn.fd = client_fd;
                    conn.client_id = GenerateId("client");
                    conn.active = true;
                    clients_[client_fd] = conn;
                }

                LOG_DEBUG("[UDS_PLUGIN_SERVER] Client connected: fd=%d", client_fd);

                // 在单独的线程中处理客户端
                std::thread client_thread(&UDSPluginServer::HandleClient, this, client_fd);
                client_thread.detach();
            }
        }

        void UDSPluginServer::HandleClient(int client_fd) {
            while (running_.load()) {
                // 使用 poll 等待数据
                struct pollfd pfd;
                pfd.fd = client_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;

                int ret = poll(&pfd, 1, 50);  // 50ms 超时
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (ret == 0) continue;  // 超时

                if (pfd.revents & (POLLHUP | POLLERR)) {
                    LOG_INFO("[UDS_PLUGIN_SERVER] Client close: fd=%d", client_fd);
                    break;  // 客户端断开
                }

                if (!(pfd.revents & POLLIN)) continue;

                // 接收消息
                std::string message;
                if (!RecvMessage(client_fd, message)) {
                    LOG_INFO("[UDS_PLUGIN_SERVER] RecvMessage failed, Client close: fd=%d", client_fd);
                    break;  // 接收失败或连接关闭
                }

                if (message.empty()) continue;

                // 处理请求
                std::cout << "[UDS_RECV] method=";
                auto method_start = message.find("\"method\"");
                if (method_start != std::string::npos) {
                    auto val_start = message.find('"', message.find(':', method_start) + 1);
                    auto val_end = message.find('"', val_start + 1);
                    if (val_start != std::string::npos && val_end != std::string::npos)
                        std::cout << message.substr(val_start + 1, val_end - val_start - 1);
                }
                std::cout << " len=" << message.size() << std::endl;
                HandleRequest(client_fd, message);
            }

            // 清理客户端
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_.erase(client_fd);
            }

            // 清理该客户端的订阅
            {
                std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                for (auto& [channel, fds] : channel_subscribers_) {
                    fds.erase(std::remove(fds.begin(), fds.end(), client_fd), fds.end());
                }
            }

            close(client_fd);
            LOG_DEBUG("[UDS_PLUGIN_SERVER] Client disconnected: fd=%d", client_fd);
        }

        bool UDSPluginServer::RecvMessage(int fd, std::string& message) {
            // 读取 4 字节长度前缀
            uint32_t length = 0;
            ssize_t n = recv(fd, &length, sizeof(length), MSG_WAITALL);
            if (n <= 0) {
                return false;  // 连接关闭或错误
            }
            if (n != sizeof(length)) {
                return false;  // 不完整的长度
            }

            // 转换网络字节序
            length = ntohl(length);

            if (length == 0) {
                message.clear();
                return true;
            }

            // 限制最大消息大小
            if (length > 10 * 1024 * 1024) {  // 10MB 限制
                LOG_ERR("[UDS_PLUGIN_SERVER] Message too large");
                return false;
            }

            // 读取消息体
            message.resize(length);
            n = recv(fd, &message[0], length, MSG_WAITALL);
            if (n != static_cast<ssize_t>(length)) {
                return false;  // 不完整的消息
            }

            return true;
        }

        // 辅助函数：确保完整发送
        bool UDSPluginServer::SendAll(int fd, const char* buffer, size_t length) {
            size_t total_sent = 0;
            const int max_retries = 3;
            const int retry_delay_ms = 10;
            int retry_count = 0;

            while (total_sent < length) {
                ssize_t n = send(fd, buffer + total_sent, length - total_sent, MSG_NOSIGNAL);

                if (n > 0) {
                    total_sent += n;
                    retry_count = 0; // 重置重试计数
                } else if (n == 0) {
                    return false; // 连接关闭
                } else {
                    int err = errno;
                    if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
                        if (++retry_count > max_retries) {
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
                        continue;
                    } else {
                        return false;
                    }
                }
            }

            return true;
        }

        bool UDSPluginServer::SendMessage(int fd, const std::string& message) {
            if (fd < 0) {
                return false;
            }

            // 4 字节长度前缀（网络字节序）
            uint32_t length = htonl(static_cast<uint32_t>(message.size()));

            LOG_INFO("[UDS_PLUGIN_SERVER] Send: %s", message.c_str());

            // 发送长度前缀
            if (!SendAll(fd, reinterpret_cast<const char*>(&length), sizeof(length))) {
                return false;
            }

            // 发送消息体
            if (!message.empty() && !SendAll(fd, message.c_str(), message.size())) {
                return false;
            }

            return true;
        }

        void UDSPluginServer::SendResponse(int fd, const UDS::UDSResponse& response) {
            std::ostringstream oss;
            oss << "{"
                << "\"request_id\":\"" << JsonEscape(response.request_id) << "\","
                << "\"status\":\"" << (response.status.empty() ? "success" : JsonEscape(response.status)) << "\","
                << "\"error_code\":" << static_cast<uint32_t>(response.error_code) << ","
                << "\"error_message\":\"" << JsonEscape(response.error_message) << "\","
                << "\"data\":" << (response.data_json.empty() ? "null" : response.data_json) << ","
                << "\"timestamp\":" << response.timestamp
                << "}";

            SendMessage(fd, oss.str());
        }

        void UDSPluginServer::SendEvent(int fd, const UDS::UDSEvent& event) {
            SendMessage(fd, event.ToJson());
        }

        bool IsContextMethod(const std::string& method)
        {
            if (method == "context.query" || method == "context.write") {
                return true;
            }
            return false;
        }

        bool IsAgentDirectMessage(const std::string& method)
        {
            if (method == "sysagent.direct") {
                return true;
            }
            return false;
        }

        bool IsSkillPlugin(const std::string& name)
        {
            if (name == "skill") {
                return true;
            }
            return false;
        }


        void UDSPluginServer::HandleRequest(int client_fd, const std::string& request_json) {
            Document doc;
            doc.Parse(request_json.c_str());

            if (doc.HasParseError()) {
                SendResponse(client_fd, {
                    .request_id = "",
                    .status = "error",
                    .error_code = UDS::UDSErrorCode::INVALID_PARAMS,
                    .error_message = "Invalid JSON",
                    .timestamp = GetCurrentTimestamp()
                });
                return;
            }

            std::string request_id = GetString(doc, "request_id");
            std::string plugin_type = GetString(doc, "plugin_type");
            std::string plugin_name = GetString(doc, "plugin_name");
            std::string method = GetString(doc, "method");
            std::string params_json;

            if (doc.HasMember("params") && doc["params"].IsObject()) {
                params_json = ValueToString(doc["params"]);
            }

            LOG_INFO("[UDS_PLUGIN_SERVER] Raw: %s Request: method=%s plugin=%s params: %s", request_json.c_str(),
                method.c_str(), plugin_name.c_str(), params_json.c_str());

            if (IsSkillPlugin(plugin_name)) {
                auto resp = HandleSkillProxyMessageInvoke(request_json);
                SendMessage(client_fd, resp);
                return;
            }

            // 路由请求
            std::string response_data;

            // 插件管理接口
            if (method == "plugin.list") {
                response_data = HandlePluginList(params_json);
            } else if (method == "plugin.info") {
                response_data = HandlePluginInfo(params_json);
            } else if (method == "plugin.init") {
                response_data = HandlePluginInit(params_json);
            }
            // LLM 接口（仅 llm.chat，旧三段式协议已全部移除）
            else if (method == "llm.chat") {
                std::cout << "[UDS_DISPATCH] -> HandleLLMChat" << std::endl;
                HandleLLMChat(params_json, client_fd);
                return;  // HandleLLMChat 自己发送事件流/响应，不走下面的统一 send
            }
            // Tool 接口
            else if (method == "tool.list") {
                response_data = HandleToolList(params_json);
            } else if (method == "tool.call") {
                response_data = HandleToolCall(params_json);
            }
        //            // RAG 接口
        //        else if (method == "rag.embedding.create") {
        //            response_data = HandleRAGEmbeddingCreate(params_json);
        //        } else if (method == "rag.embedding.vectorize") {
        //            response_data = HandleRAGEmbeddingVectorize(params_json);
        //        } else if (method == "rag.embedding.batch_vectorize") {
        //            response_data = HandleRAGEmbeddingBatchVectorize(params_json);
        //        } else if (method == "rag.datasource.create") {
        //            response_data = HandleRAGDataSourceCreate(params_json);
        //        } else if (method == "rag.rerank.create") {
        //            response_data = HandleRAGRerankCreate(params_json);
        //        } else if (method == "rag.retriever.create") {
        //            response_data = HandleRAGRetrieverCreate(params_json);
        //        } else if (method == "rag.retrieve") {
        //            response_data = HandleRAGRetrieve(params_json);
        //        }

            // 上下文接口
            else if (IsContextMethod(method)) {
                response_data = HandleContextServiceInvoke(method, params_json);
            } else if (IsAgentDirectMessage(method)) {
                response_data = HandleDirectMessageInvoke(params_json);
            }
                // 事件订阅
            else if (method == "event.subscribe") {
                response_data = HandleEventSubscribe(params_json, client_fd);
            } else if (method == "event.unsubscribe") {
                response_data = HandleEventUnsubscribe(params_json);
            }
                // 心跳
            else if (method == "heartbeat") {
                response_data = HandleHeartbeat();
            }
            else {
                SendResponse(client_fd, {
                    .request_id = request_id,
                    .status = "error",
                    .error_code = UDS::UDSErrorCode::METHOD_NOT_FOUND,
                    .error_message = "Method not found: " + method,
                    .timestamp = GetCurrentTimestamp()
                });
                return;
            }
            Document resp_doc;
            resp_doc.Parse(response_data.c_str());

            if (resp_doc.HasMember("error_code") && resp_doc["error_code"].IsUint()) {
                uint32_t ec = resp_doc["error_code"].GetUint();
                std::string error_msg = GetString(resp_doc, "error_message", "");

                SendResponse(client_fd, {
                    .request_id = request_id,
                    .status = ec == 0 ? "success" : "error",
                    .error_code = static_cast<UDS::UDSErrorCode>(ec),
                    .error_message = error_msg,
                    .data_json = response_data,
                    .timestamp = GetCurrentTimestamp()
                });
            } else {
                SendResponse(client_fd, {
                    .request_id = request_id,
                    .status = "success",
                    .error_code = UDS::UDSErrorCode::SUCCESS,
                    .data_json = response_data,
                    .timestamp = GetCurrentTimestamp()
                });
            }
        }

// ==================== 插件管理 ====================

    void UDSPluginServer::RegisterLLMPlugin(const std::string& name,
        std::shared_ptr<AgentFrm::Plugin::LLMPlugin> plugin) {
        std::lock_guard<std::mutex> lock(plugins_mutex_);
        llm_plugins_[name] = plugin;
        LOG_INFO("[UDS_PLUGIN_SERVER] Registered LLM plugin: %s", name.c_str());

        // 初始化 llm plugin
        AgentFrm::Plugin::MMLLMIntfLogReg mmLLMIntfLogReg;
    }

    void UDSPluginServer::RegisterToolPlugin(const std::string& name,
        std::shared_ptr<AgentFrm::Plugin::ToolPlugin> plugin) {
        std::lock_guard<std::mutex> lock(plugins_mutex_);
        tool_plugins_[name] = std::move(plugin);
        LOG_INFO("[UDS_PLUGIN_SERVER] Registered Tool plugin: %s", name.c_str());
    }

    void UDSPluginServer::RegisterContextPlugin(const std::string& name, ContextPlugin::ContextPlugin& plugin) {
        std::lock_guard<std::mutex> lock(plugins_mutex_);
        context_plugins_[name] = &plugin;
        LOG_INFO("[UDS_PLUGIN_SERVER] Registered Context plugin: %s", name.c_str());
        ContextPlugin::InitParams initParams;
        plugin.Init(initParams);
    }

    void UDSPluginServer::UnregisterPlugin(const std::string& name) {
        std::lock_guard<std::mutex> lock(plugins_mutex_);
        llm_plugins_.erase(name);
        LOG_INFO("[UDS_PLUGIN_SERVER] Unregistered plugin: %s", name.c_str());
    }

// ==================== 插件管理接口实现 ====================

    std::string UDSPluginServer::HandlePluginList(const std::string& params_json) {
        (void)params_json;
        rapidjson::StringBuffer buffer;
        Writer<rapidjson::StringBuffer> writer(buffer);

        writer.StartObject();
        writer.Key("plugins");
        writer.StartArray();

        {
            std::lock_guard<std::mutex> lock(plugins_mutex_);

            // LLM 插件
            for (const auto& [name, plugin] : llm_plugins_) {
                writer.StartObject();
                writer.Key("name"); writer.String(name.c_str());
                writer.Key("type"); writer.String("LLM");
                writer.Key("tags");
                writer.StartArray();
                // TODO: 添加标签
                writer.EndArray();
                writer.Key("version"); writer.String("1.0.0");
                writer.Key("description"); writer.String("LLM Plugin");
                writer.EndObject();
            }
        }

        writer.EndArray();
        writer.EndObject();

        return buffer.GetString();
    }

    std::string UDSPluginServer::HandlePluginInfo(const std::string& params_json) {
        Document doc;
        doc.Parse(params_json.c_str());
        std::string plugin_name = GetString(doc, "plugin_name");

        std::lock_guard<std::mutex> lock(plugins_mutex_);

        // 检查各类型插件
        if (llm_plugins_.count(plugin_name)) {
            return "{\"name\":\"" + JsonEscape(plugin_name) + "\",\"type\":\"LLM\","
                                                              "\"version\":\"1.0.0\",\"capabilities\":[\"complete\",\"stream\",\"context\"]}";
        }

        return "{\"error_code\":4,\"error_message\":\"Plugin not found\"}";
    }

    std::string UDSPluginServer::HandlePluginInit(const std::string& params_json) {
        Document doc;
        doc.Parse(params_json.c_str());
        std::string plugin_name = GetString(doc, "plugin_name");
        std::string config_path = GetString(doc, "config_path");

        std::lock_guard<std::mutex> lock(plugins_mutex_);

        // 初始化 LLM 插件
        if (llm_plugins_.count(plugin_name)) {
            AgentFrm::Plugin::MMLLMIntfLogReg logReg;
            logReg.modelId = plugin_name;
            logReg.levelDebug = 1;
            logReg.levelInfo = 1;
            logReg.levelWarn = 1;
            logReg.levelError = 1;
            logReg.levelFatal = 1;

            if (llm_plugins_[plugin_name]->Init(config_path, logReg)) {
                return "{\"initialized\":true}";
            }
            return "{\"error_code\":1,\"error_message\":\"Failed to initialize plugin\"}";
        }

        return "{\"error_code\":4,\"error_message\":\"Plugin not found\"}";
    }


    Document doc;
    doc.Parse(params_json.c_str());

    // 提取 request 对象（完整 OpenAI 格式请求体）
    std::string request_body_str;
    bool stream_mode = true;
    if (doc.HasMember("request") && doc["request"].IsObject()) {
        request_body_str = ValueToString(doc["request"]);
        // 从 request 中判断是否为流式
        Document req_doc;
        req_doc.Parse(request_body_str.c_str());
        if (req_doc.HasMember("stream") && req_doc["stream"].IsBool()) {
            stream_mode = req_doc["stream"].GetBool();
        }
    }

    LOG_INFO("[UDSPluginServer] HandleLLMChat ENTER: client_fd=%d, stream=%d, body_len=%zu",
        client_fd, stream_mode, request_body_str.size());

    if (request_body_str.empty()) {
        SendResponse(client_fd, {
            .request_id = GetString(doc, "request_id"),
            .status = "error",
            .error_code = UDS::UDSErrorCode::INVALID_PARAMS,
            .error_message = "Missing request body",
            .timestamp = GetCurrentTimestamp()
        });
        return "";
    }

    if (stream_mode) {
        // ── 流式模式 ──
        // request_body_str 中已由客户端设置 "stream": true，直接转发
        uint32_t error_code = 0;
        bool success = IMInferServiceClient::Instance()->ForwardOpenAIStream(
            request_body_str,
            [this, client_fd, request_id](const OpenAIChunkResult& result) {
                if (result.is_done) {
                    return;  // [DONE] 无需推送
                }
                if (result.is_error) {
                    UDS::UDSEvent event;
                    event.event_id = GenerateId("evt");
                    event.event_type = "llm_error";
                    event.channel = "llm.chat";
                    event.request_id = request_id;
                    event.data_json = "{\"error_message\":\"" + JsonEscape(result.error_message) + "\"}";
                    event.done = true;
                    event.timestamp = GetCurrentTimestamp();
                    SendEvent(client_fd, event);
                    return;
                }
                if (!result.content.empty()) {
                    // llm_token 事件
                    UDS::UDSEvent token_evt;
                    token_evt.event_id = GenerateId("evt");
                    token_evt.event_type = "llm_token";
                    token_evt.channel = "llm.chat";
                    token_evt.request_id = request_id;
                    token_evt.data_json = "{\"token\":\"" + JsonEscape(result.content) + "\"}";
                    token_evt.done = false;
                    token_evt.timestamp = GetCurrentTimestamp();
                    SendEvent(client_fd, token_evt);
                }
                if (!result.tool_calls_json.empty()) {
                    // llm_tool_calls 事件
                    UDS::UDSEvent tc_evt;
                    tc_evt.event_id = GenerateId("evt");
                    tc_evt.event_type = "llm_tool_calls";
                    tc_evt.channel = "llm.chat";
                    tc_evt.request_id = request_id;
                    tc_evt.data_json = "{\"tool_calls\":" + result.tool_calls_json + "}";
                    tc_evt.done = false;
                    tc_evt.timestamp = GetCurrentTimestamp();
                    SendEvent(client_fd, tc_evt);
                }
                // 当 finish_reason 非空或 usage 有值时发 llm_final
                if (!result.finish_reason.empty() || result.total_tokens > 0) {
                    UDS::UDSEvent final_evt;
                    final_evt.event_id = GenerateId("evt");
                    final_evt.event_type = "llm_final";
                    final_evt.channel = "llm.chat";
                    final_evt.request_id = request_id;
                    // 构造 data_json
                    std::string data = "{"
                        "\"answer\":\"" + JsonEscape(result.content) + "\","
                        "\"finish_reason\":\"" + JsonEscape(result.finish_reason) + "\","
                        "\"usage\":{"
                            "\"prompt_tokens\":" + std::to_string(result.prompt_tokens) + ","
                            "\"completion_tokens\":" + std::to_string(result.completion_tokens) + ","
                            "\"total_tokens\":" + std::to_string(result.total_tokens) +
                        "}}";
                    final_evt.data_json = data;
                    final_evt.done = true;
                    final_evt.timestamp = GetCurrentTimestamp();
                    SendEvent(client_fd, final_evt);
                }
            },
            error_code
        );

        if (!success && error_code != 0) {
            LOG_ERR("[UDSPluginServer] HandleLLMChat FAILED: error_code=%u", error_code);
            SendResponse(client_fd, {
                .request_id = GetString(doc, "request_id"),
                .status = "error",
                .error_code = UDS::UDSErrorCode::ERROR,
                .error_message = "ForwardOpenAIStream failed",
                .timestamp = GetCurrentTimestamp()
            });
        }
    } else {
        // ── 非流式模式 ──
        OpenAIChunkResult result = IMInferServiceClient::Instance()->ForwardOpenAIComplete(
            request_body_str, 60
        );

        if (result.is_error) {
            SendResponse(client_fd, {
                .request_id = GetString(doc, "request_id"),
                .status = "error",
                .error_code = UDS::UDSErrorCode::ERROR,
                .error_message = result.error_message,
                .timestamp = GetCurrentTimestamp()
            });
        } else {
            // 构造完整响应 data_json
            std::string data = "{"
                "\"content\":\"" + JsonEscape(result.content) + "\","
                "\"finish_reason\":\"" + JsonEscape(result.finish_reason) + "\"";
            if (!result.tool_calls_json.empty()) {
                data += ",\"tool_calls\":" + result.tool_calls_json;
            }
            data += ",\"usage\":{"
                "\"prompt_tokens\":" + std::to_string(result.prompt_tokens) + ","
                "\"completion_tokens\":" + std::to_string(result.completion_tokens) + ","
                "\"total_tokens\":" + std::to_string(result.total_tokens) +
            "}}";

            SendResponse(client_fd, {
                .request_id = GetString(doc, "request_id"),
                .status = "success",
                .error_code = UDS::UDSErrorCode::SUCCESS,
                .data_json = data,
                .timestamp = GetCurrentTimestamp()
            });
        }
    }

    LOG_INFO("[UDSPluginServer] HandleLLMChat EXIT: client_fd=%d, stream=%d", client_fd, stream_mode);
    return "";

    // ==================== Tool 接口实现 ====================

    std::string UDSPluginServer::HandleToolList(const std::string& params_json) {
        Document doc;
        doc.Parse(params_json.c_str());
        std::string plugin_name = GetString(doc, "plugin_name");

        // 获取插件
        std::shared_ptr<AgentFrm::Plugin::ToolPlugin> plugin;
        {
            std::lock_guard<std::mutex> lock(plugins_mutex_);
            auto it = tool_plugins_.find(plugin_name);
            if (it == tool_plugins_.end()) {
                return "{\"error_code\":4,\"error_message\":\"Plugin not found\"}";
            }
            plugin = it->second;
        }

        // 列出工具
        std::vector<AgentFrm::Plugin::MMListToolResult> tools;
        uint32_t error_code = plugin->ListTools(tools);

        if (error_code != 0) {
            return "{\"error_code\":" + std::to_string(error_code) + ",\"error_message\":\"List tools failed\"}";
        }

        // 转换为
        rapidjson::StringBuffer buffer;
        Writer<rapidjson::StringBuffer> writer(buffer);

        writer.StartObject();
        writer.Key("tools");
        writer.StartArray();

        for (const auto& tool : tools) {
            writer.StartObject();
            writer.Key("name"); writer.String(tool.name.c_str());
            writer.Key("description"); writer.String(tool.description.c_str());
            writer.Key("input_schema");
            writer.RawValue(tool.inputSchema.c_str(), tool.inputSchema.size(), rapidjson::kObjectType);
            writer.Key("output_schema");
            writer.RawValue(tool.outputSchema.c_str(), tool.outputSchema.size(), rapidjson::kObjectType);
            writer.EndObject();
        }

        writer.EndArray();
        writer.EndObject();

        return buffer.GetString();
    }

    std::string UDSPluginServer::HandleToolCall(const std::string& params_json) {
        Document doc;
        doc.Parse(params_json.c_str());

        std::string plugin_name = GetString(doc, "plugin_name");
        std::string tool_name = GetString(doc, "tool_name");

        std::string params;
        if (doc.HasMember("params") && doc["params"].IsObject()) {
            params = ValueToString(doc["params"]);
        }

        // 获取插件
        std::shared_ptr<AgentFrm::Plugin::ToolPlugin> plugin;
        {
            std::lock_guard<std::mutex> lock(plugins_mutex_);
            auto it = tool_plugins_.find(plugin_name);
            if (it == tool_plugins_.end()) {
                return "{\"error_code\":4,\"error_message\":\"Plugin not found\"}";
            }
            plugin = it->second;
        }

        // 调用工具
        AgentFrm::Plugin::MMCallToolResult result;
        uint32_t error_code = plugin->CallTool(tool_name, params, result);

        rapidjson::StringBuffer buffer;
        Writer<rapidjson::StringBuffer> writer(buffer);

        writer.StartObject();
        writer.Key("content");
        writer.StartArray();

        for (const auto& content : result.content) {
            writer.StartObject();
            writer.Key("type");
            switch (content.type) {
                case AgentFrm::Plugin::MMCallToolResultType::TEXT:
                    writer.String("text");
                    break;
            }
            writer.Key("text");
            writer.RawValue(content.textContent.text.c_str(),
                           content.textContent.text.size(),
                           rapidjson::kObjectType);
            writer.EndObject();
        }

        writer.EndArray();
        writer.Key("error_code");
        writer.Uint(error_code);
        writer.EndObject();

        return buffer.GetString();
    }

// ==================== RAG 接口实现 ====================

    std::string UDSPluginServer::HandleRAGRetrieveSearch(const std::string& params_json)
    {
        Document doc;
        doc.Parse(params_json.c_str());

        auto threshold = GetDouble(doc, "thresh_hold");
        std::string query = GetString(doc, "query");


        auto queryOption = QueryOption::Builder(TableQueryType::VECTOR)
            .WithBaseSchemaName(SCEMA_NAME)
            .WithVectorDistanceAlgorithm(VectorDistanceAlgorithm::L2)
            .WithVectorField("query_vector")
            .WithVectorTopK(1)
            .WithBaseOrder(QueryOption::QueryOrder::ASC)
            .WithBaseFilters(FilterOption::Builder()
                .WithFieldName("scene_type")
                .WithEQ()
                .WithValue(std::make_shared<VarCharValue>("2"))
                .Build())
            .WithVectorQuery(query)
            .WithBaseSelectFields({"scene_type", "query", "content"})
            .WithVectorReturnDistance()
            .Build();

        auto ragResult = std::shared_ptr<RAGResult>(new RAGResult{false, "", "", ""});
        // 注意: SearchAsync 是 fire-and-forget，不等待回调。
        // shared_ptr 保证回调触发时 ragResult 仍然有效，即使函数已返回。
        GaussClient::Instance()->SearchAsync(
            "safetyfence", queryOption,
            [threshold, ragResult](DBOperateCode code, const std::vector<std::vector<std::string>>& result) {
                if (!result.empty() && stof(result[0][0]) < threshold) {
                    Json::Value root;
                    std::map<std::string, std::string> params;
                    AisfPlat::JsonParser::Parse(result[0][CMFrm::Constants::Const3].c_str(), root);
                    if (root.isMember("params")) {
                        Json::Value::Members members = root["params"].getMemberNames();
                        for (const auto& member : members) {
                            params.insert({member.c_str(), root["params"][member].asString()});
                        }
                    }
                    *ragResult = RAGResult{true, "", SafetyFence::parseIntentName(result[0][3]), "", params};
                }
            });

        return ragResult->ToJson();
    }

// ==================== 消息直出接口实现 ====================
    std::string UDSPluginServer::HandleDirectMessageInvoke(const std::string& params_json)
    {
        std::string response;
        framework::MMAgentMsgQue::Message message;
        message.content = params_json;
        std::shared_ptr<AccessCtrl> accessCtrl;
        auto msg = AgentFlowManager::ConvertMMAgentMsgToAccessCtrl(message);
        SysCallBack::SysAgentMsgCallBack(msg.msg, msg.dialogueId, msg.sessionId);
        return R"({"result": "success" })";
    }

// ==================== 事件订阅 ====================

    std::string UDSPluginServer::HandleContextServiceInvoke(const std::string& method, const string& params_json)
    {
        Document doc;
        doc.Parse(params_json.c_str());
        if (doc.HasParseError()) {
            return "{\"error_code\":1,\"error_message\":\"Invalid JSON params\"}";
        }

        if (method == "context.query") {
            ContextManager::ContextRequest request;
            request.userId = GetString(doc, "user_id");
            request.subUserId = GetString(doc, "sub_user_id");
            request.sessionId = GetString(doc, "session_id");
            request.currentAgent = GetString(doc, "current_agent");
            request.queryType = GetString(doc, "query_type");
            request.query = GetString(doc, "query");
            request.rewriteQuery = GetString(doc, "rewrite_query");
            request.limitQARounds = static_cast<int>(GetInt64(doc, "limit_qa_rounds", 20));
            request.maxQATokens = static_cast<int>(GetInt64(doc, "max_qa_tokens", 40960));
            request.needLongMemory = GetString(doc, "need_long_memory", "true");
            request.needStartGetMemory = GetString(doc, "need_start_get_memory", "false");

            struct ContextQueryState {
                std::mutex mutex;
                std::condition_variable cv;
                bool done = false;
                bool success = false;
                std::shared_ptr<ContextManager::ContextResult> result;
            };
            auto state = std::make_shared<ContextQueryState>();

            ContextManager::GetContext(request, [state](bool isSuccess, std::shared_ptr<ContextManager::ContextResult> ctxResult) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->success = isSuccess;
                state->result = ctxResult;
                state->done = true;
                state->cv.notify_one();
            });

            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait_for(lock, std::chrono::milliseconds(5000), [&state] { return state->done; });
            }

            if (!state->done || !state->success) {
                return "{\"error_code\":2,\"error_message\":\"GetContext failed or timeout\"}";
            }
            LOG_INFO("[CTX_QUERY_RESULT] user_id=%s query_type=%s result=%s",
                     request.userId.c_str(), request.queryType.c_str(),
                     state->result->result.c_str());
            return state->result->result;
        }
        else if (method == "context.write") {
            std::string userId = GetString(doc, "user_id");
            std::string subUserId = GetString(doc, "sub_user_id");
            std::string sessionId = GetString(doc, "session_id");
            std::string agentName = GetString(doc, "agent_name");
            std::string label = GetString(doc, "label");
            Json::Value context;
            if (doc.HasMember("context") && doc["context"].IsArray()) {
                std::string contextStr = ValueToString(doc["context"]);
                Json::Reader reader;
                reader.parse(contextStr, context);
                LOG_INFO("[CTX_WRITE_CONTENT] user_id=%s agent=%s label=%s context=%s",
                         userId.c_str(), agentName.c_str(), label.c_str(), contextStr.c_str());
            }

            struct ContextWriteState {
                std::mutex mutex;
                std::condition_variable cv;
                bool done = false;
                bool success = false;
                std::vector<std::string> contextIds;
            };
            auto state = std::make_shared<ContextWriteState>();

            ContextManager::AddContext(userId, subUserId, sessionId, agentName, label, context, [state](bool isSuccess, std::vector<std::string> ids) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->success = isSuccess;
                state->contextIds = std::move(ids);
                state->done = true;
                state->cv.notify_one();
            });

            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait_for(lock, std::chrono::milliseconds(5000), [&state] { return state->done; });
            }

            if (!state->done) {
                return "{\"error_code\":2,\"error_message\":\"AddContext timeout\"}";
            }
            if (!state->success) {
                return "{\"error_code\":3,\"error_message\":\"AddContext failed\"}";
            }

            rapidjson::StringBuffer buffer;
            Writer<rapidjson::StringBuffer> writer(buffer);
            writer.StartObject();
            writer.Key("context_id");
            writer.StartArray();
            for (const auto& id : state->contextIds) {
                writer.String(id.c_str());
            }
            writer.EndArray();
            writer.EndObject();
            return buffer.GetString();
        }

        return "{\"error_code\":4,\"error_message\":\"Unknown context method\"}";
    }

// ==================== 事件订阅 ====================

    std::string UDSPluginServer::HandleEventSubscribe(const std::string& params_json, int client_fd) {
        Document doc;
        doc.Parse(params_json.c_str());

        std::string subscription_id = GenerateId("sub");

        if (doc.HasMember("channels") && doc["channels"].IsArray()) {
            const auto& channels = doc["channels"];
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);

            for (rapidjson::SizeType i = 0; i < channels.Size(); i++) {
                if (channels[i].IsString()) {
                    std::string channel = channels[i].GetString();
                    channel_subscribers_[channel].push_back(client_fd);
                }
            }
        }

        return "{\"subscribed\":true,\"subscription_id\":\"" + subscription_id + "\"}";
    }

        std::string UDSPluginServer::HandleEventUnsubscribe(const std::string& params_json) {
            Document doc;
            doc.Parse(params_json.c_str());
            std::string subscription_id = GetString(doc, "subscription_id");

            // TODO: 实现取消订阅

            return "{\"unsubscribed\":true}";
        }

// ==================== 心跳 ====================

    std::string UDSPluginServer::HandleHeartbeat() {
        return "{\"pong\":true,\"server_time\":" + std::to_string(GetCurrentTimestamp()) + "}";
    }

// ==================== 辅助方法 ====================

    void UDSPluginServer::CleanupSocket() {
        if (!config_.socket_path.empty()) {
            unlink(config_.socket_path.c_str());
        }
    }

    std::string UDSPluginServer::GenerateId(const std::string& prefix) {
        uint64_t id = id_counter_.fetch_add(1);
        return prefix + "_" + std::to_string(GetCurrentTimestamp()) + "_" + std::to_string(id);
    }

    int64_t UDSPluginServer::GetCurrentTimestamp() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    std::string UDSPluginServer::BuildSuccessResponse(const std::string& request_id,
        const std::string& data_json) {
        return "{\"request_id\":\"" + request_id + "\",\"status\":\"success\","
                                                   "\"error_code\":0,\"data\":" + data_json + ",\"timestamp\":" +
            std::to_string(GetCurrentTimestamp()) + "}";
    }

    std::string UDSPluginServer::BuildErrorResponse(const std::string& request_id,
        UDS::UDSErrorCode code,
        const std::string& message) {
        return "{\"request_id\":\"" + request_id + "\",\"status\":\"error\","
                                                   "\"error_code\":" + std::to_string(static_cast<uint32_t>(code)) + ","
                                                                                                                     "\"error_message\":\"" + JsonEscape(message) + "\","
                                                                                                                                                                    "\"timestamp\":" + std::to_string(GetCurrentTimestamp()) + "}";
    }

    std::string UDSPluginServer::BuildEventMessage(const UDS::UDSEvent& event) {
        return "{\"event_id\":\"" + event.event_id + "\","
                                                     "\"event_type\":\"" + event.event_type + "\","
                                                                                              "\"channel\":\"" + event.channel + "\","
                                                                                                                                 "\"data\":" + event.data_json + ","
                                                                                                                                                                 "\"done\":" + (event.done ? "true" : "false") + ","
                                                                                                                                                                                                                 "\"timestamp\":" + std::to_string(event.timestamp) + "}";
    }

    std::string UDSPluginServer::HandleLocalMessageSendTest(const string& params_json)
    {
        auto accessCtrl = std::make_shared<AccessCtrl>();
        accessCtrl->header.messageType = "UserTaskManagement";
        accessCtrl->taskAssignment.header.messageType = "UserTaskManagement";
        accessCtrl->taskAssignment.data.userInputText = "今天天气怎么样？";
        AgentFlowManager::GetAgentFlowManager()->SendMsg(accessCtrl);
        return std::string();
    }


    std::string UDSPluginServer::HandleSkillProxyMessageInvoke(const string& request_json)
    {
        auto skillPluginPtr = AgentFrm::Plugin::SkillPlugin::Instance();
        auto response = skillPluginPtr->HandleMessage(request_json);
        if(!response) {
            LOG_ERR("[UDSPluginServer] handle message failed json is %s", request_json.c_str());
            return BuildErrorResponse("", UDS::UDSErrorCode::ERROR, "");
        }
        return *response;
    }


std::string RAGResult::ToJson()
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

        writer.StartObject();

        writer.Key("similar");
        writer.Bool(similar);

        writer.Key("category");
        writer.String(category.c_str());

        writer.Key("intent");
        writer.String(intent.c_str());

        writer.Key("data");
        writer.String(data.c_str());

        writer.Key("params");
        writer.StartObject();
        for (const auto& param : params) {
            writer.Key(param.first.c_str());
            writer.String(param.second.c_str());
        }
        writer.EndObject();

        writer.EndObject();

        return buffer.GetString();
    }
}  // namespace agentflow