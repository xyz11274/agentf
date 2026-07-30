/**
 * coremind_bridge — C++ sidecar as the single external entry point.
 *
 * In production, this is the ONLY port exposed to the outside world.
 * It handles:
 *   1. CoreMind service discovery (resolve internal service addresses)
 *   2. Trusted request forwarding to the Python Bridge
 *   3. Health checks / capability advertisement
 *
 * Architecture:
 *   External request → Sidecar (:23767) → Bridge (:8199) → agentflowd (:17891)
 *
 * Build:
 *   # Production (Huawei CI/CD, requires CoreMind C++ SDK):
 *   cmake -B build -DCOREMIND_ENABLED=ON -DPLATFORM_DIR=/path/to/coremind
 *   cmake --build build
 *
 *   # Local / E2E (no SDK required, stub service discovery):
 *   cmake -B build -DCOREMIND_ENABLED=OFF
 *   cmake --build build
 *   # Then inject fake instances via COREMIND_STUB_SERVICES env var
 *
 * Run:
 *   ./coremind_bridge --listen 0.0.0.0:23767 \
 *                     --bridge-url http://127.0.0.1:8199
 */

#include <httplib.h>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <json/json.h>

#include "resolve_handler.h"
#include "uds_model_proxy.h"
#include "proxy_handler.h"
#include "sidecar_router.h"

// CoreMind framework headers — only available in Huawei build environment
#ifdef COREMIND_ENABLED
#include <framework_com/http_client_factory.h>
#include <framework_starter/framework_starter.h>
#endif

// ── Global state ──
static std::atomic<httplib::Server*> g_http_server{nullptr};

#ifdef COREMIND_ENABLED
static std::shared_ptr<SidecarRouter> g_sidecar_router;
#endif

static void signal_handler(int sig) {
    const char* name = (sig == SIGTERM) ? "SIGTERM" : (sig == SIGINT) ? "SIGINT" : "SIG?";
    std::cerr << "[coremind_bridge] received " << name << ", shutting down..." << std::endl;
    if (auto* svr = g_http_server.load()) {
        svr->stop();
    }
#ifdef COREMIND_ENABLED
    // CoreMind 框架的 WaitProcessExit() 会处理框架级 shutdown
    std::cerr << "[coremind_bridge] CoreMind framework will shut down" << std::endl;
#endif
}

// ── JSON helpers ──
namespace {
static Json::StreamWriterBuilder json_writer() {
    static Json::StreamWriterBuilder w = [] {
        Json::StreamWriterBuilder b;
        b["indentation"] = "";
        b["emitUTF8"] = true;
        return b;
    }();
    return w;
}

static void set_json_response(httplib::Response& res, const Json::Value& json, int status = 200) {
    res.status = status;
    res.set_content(Json::writeString(json_writer(), json), "application/json");
}

static std::string pretty_json(const std::string& raw) {
    if (raw.empty()) return raw;
    Json::CharReaderBuilder reader;
    Json::Value val;
    std::string errors;
    std::istringstream input(raw);
    if (Json::parseFromStream(reader, input, &val, &errors)) {
        return val.toStyledString();
    }
    return raw;
}
}  // namespace

// ── Forward declarations ──
static void configure_server(httplib::Server& server, const std::string& bridge_url);

static Json::Value service_snapshot(const std::string& name) {
    const auto result = resolve_service(name);
    Json::Value snapshot;
    snapshot["status"] = result.success ? "resolved" : "unavailable";
    snapshot["instances"] = Json::Value(Json::arrayValue);
    for (const auto& instance : result.instances) {
        Json::Value endpoint;
        endpoint["host"] = instance.ip;
        endpoint["port"] = instance.port;
        snapshot["instances"].append(endpoint);
    }
    if (!result.success) {
        snapshot["error_code"] = "SERVICE_DISCOVERY_EMPTY";
    }
    return snapshot;
}

static std::vector<std::string> split_services(const char* value) {
    std::vector<std::string> services;
    if (value == nullptr) return services;
    std::istringstream input(value);
    std::string service;
    while (std::getline(input, service, ',')) {
        if (!service.empty()) services.push_back(service);
    }
    return services;
}

struct BridgeTarget {
    std::string origin;
    std::string path_prefix;
};

static BridgeTarget parse_bridge_target(const std::string& bridge_url) {
    const auto scheme = bridge_url.find("://");
    const auto authority_start = scheme == std::string::npos ? 0 : scheme + 3;
    const auto path_start = bridge_url.find('/', authority_start);
    if (path_start == std::string::npos) {
        return {bridge_url, ""};
    }

    auto prefix = bridge_url.substr(path_start);
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.pop_back();
    }
    return {bridge_url.substr(0, path_start), prefix == "/" ? "" : prefix};
}

static void forward_to_bridge(
    const std::string& bridge_url,
    const httplib::Request& req,
    httplib::Response& res) {

    // 日志：记录入站请求（method + path + 缩进格式化的请求体）
    {
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path
                  << " | body=\n" << pretty_json(req.body) << std::endl;
    }

    Json::CharReaderBuilder reader_builder;
    Json::Value original_request;
    std::string errors;
    std::istringstream input(req.body);
    if (!Json::parseFromStream(reader_builder, input, &original_request, &errors) ||
        !original_request.isObject()) {
        res.status = 400;
        res.set_content(R"({"error":"invalid JSON request"})", "application/json");
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 400 | body={\"error\":\"invalid JSON request\"}" << std::endl;
        return;
    }

    // 从 AccessCtrl 格式提取字段（与 HandleUserRequest 一致）
    auto header = original_request["Header"];
    auto data = original_request["Data"];
    std::string user_name = header.get("UserName", "").asString();
    std::string client_session_id = header.get("ClientSessionId",
                               header.get("SessionId", "")).asString();
    std::string user_input_text = data.get("UserInputText", "").asString();
    std::string sys_agent_entity = header.get("SysAgentEntity", "").asString();

    // 构建多模态 content
    Json::Value content_parts(Json::arrayValue);
    if (!user_input_text.empty()) {
        Json::Value text_part;
        text_part["type"] = "text";
        text_part["text"] = user_input_text;
        content_parts.append(text_part);
    }
    auto user_input_obj = data["UserInputObject"]["data"]["content"];
    if (user_input_obj.isArray()) {
        for (const auto& item : user_input_obj) {
            std::string item_type = item.get("type", "").asString();
            if (item_type == "image") {
                Json::Value image_part;
                image_part["type"] = "image_url";
                image_part["image_url"]["url"] = item.get("data", "").asString();
                content_parts.append(image_part);
            }
        }
    }
    // content 直接传 ContentPart 数组，不包装 messages 结构
    // Bridge 的 _chat_turn_events 会自行包装 role/user
    std::string content_str;
    if (content_parts.size() == 1 && content_parts[0].isMember("text")) {
        // 纯文本场景：直接传字符串
        content_str = content_parts[0]["text"].asString();
    } else {
        // 多模态场景：传 ContentPart JSON 数组
        content_str = Json::writeString(json_writer(), content_parts);
    }

    // 构造 REST API 请求体
    static std::atomic<unsigned long long> request_counter{0};
    auto request_id = req.get_header_value("X-Request-Id");
    if (request_id.empty()) {
        request_id = "sidecar-" + std::to_string(++request_counter);
    }

    Json::Value rest_body;
    rest_body["user_id"] = user_name;
    rest_body["client_id"] = client_session_id;
    rest_body["content"] = content_str;

    // 从 bridge_url 提取 origin 和 path prefix
    const auto target = parse_bridge_target(bridge_url);
    const char* env_agent = std::getenv("AF_BR_COREMIND_AGENT_ID");
    std::string agent_id = env_agent ? env_agent : "outfit-assistant";

    if (sys_agent_entity.empty()) {
        std::cerr << "[coremind_bridge] WARN no SysAgentEntity, callbacks will not be delivered"
                  << std::endl;
    }

    // ══════════════════════════════════════════════════════════════
    //  always async: 202 + 后台 SSE 流式转发 + POST 回调
    // ══════════════════════════════════════════════════════════════
    std::string ts_body = R"({"status":"accepted","request_id":")" + request_id + R"("})";
    res.set_content(ts_body, "application/json");
    std::cerr << "[coremind_bridge] ASYNC " << req.path << " req=" << request_id
              << " callback=" << (sys_agent_entity.empty() ? "(none)" : sys_agent_entity) << std::endl;

    // 后台线程处理 SSE 流 + POST 回调
        std::thread([=]() {
            std::string stream_path = target.path_prefix + "/api/chat/" + agent_id + "?stream=true";
            httplib::Client cli(target.origin.c_str());
            cli.set_read_timeout(360, 0);
            httplib::Headers hdrs;
            hdrs.emplace("Content-Type", "application/json");
            hdrs.emplace("X-Request-Id", request_id);

            std::string sse_buffer;
            std::string accumulated;
            std::string last_sent;
            bool has_content = false;

            // POST 回调辅助函数
            auto post_callback = [&](const std::string& stream_status, const std::string& text, int flag = 0) {
                Json::Value cb;
                cb["Data"]["MessageType"] = "UserOutput";
                cb["Data"]["StreamStatus"] = stream_status;
                cb["Data"]["UserOutputFlag"] = flag;
                cb["Data"]["UserOutputText"] = text;
                cb["Header"]["MessageType"] = "DataExchange";
                cb["Header"]["TaskID"] = request_id;
                cb["Header"]["Sender"] = "SysAgent";
                cb["Header"]["Receiver"] = "APP";
                cb["Header"]["SeqNum"] = request_id;
                cb["Header"]["UserName"] = user_name;
                cb["Header"]["ClientSessionId"] = client_session_id;
                std::string cb_body = Json::writeString(json_writer(), cb);
                std::cerr << "[coremind_bridge] CBF_CALLBACK req=" << request_id
                          << " status=" << stream_status << " flag=" << flag
                          << " text_len=" << text.size()
                          << " body=\n" << cb.toStyledString() << std::endl;
                httplib::Client cb_cli(sys_agent_entity.c_str());
                auto cb_res = cb_cli.Post("/coremind/systemagent/v1/callback",
                            {{"Content-Type", "application/json"}},
                            cb_body, "application/json");
                if (cb_res) {
                    std::cerr << "[coremind_bridge] CBF_RESP req=" << request_id
                              << " http=" << cb_res->status << std::endl;
                } else {
                    auto err = cb_res.error();
                    std::cerr << "[coremind_bridge] CBF_ERR req=" << request_id
                              << " error=" << httplib::to_string(err) << std::endl;
                }
            };

            std::string body_str = Json::writeString(json_writer(), rest_body);
            auto result = cli.Post(stream_path.c_str(), hdrs, body_str, "application/json",
                [&](const char* data, size_t len) {
                    sse_buffer.append(data, len);
                    size_t pos;
                    while ((pos = sse_buffer.find('\n')) != std::string::npos) {
                        std::string line = sse_buffer.substr(0, pos);
                        sse_buffer.erase(0, pos + 1);
                        if (line.substr(0, 6) == "data: ") {
                            std::string payload = line.substr(6);
                            if (payload == "[DONE]") continue;
                            // 打印原始 Bridge SSE 数据（截断前 500 字符，避免日志过大）
                            std::string sse_preview = payload.substr(0, 500);
                            if (payload.size() > 500) sse_preview += "... (truncated)";
                            std::cerr << "[coremind_bridge] SSE_RAW len=" << payload.size()
                                      << " data=" << sse_preview << std::endl;
                            Json::Value sse_json;
                            Json::CharReaderBuilder rdr;
                            std::istringstream sse_in(payload);
                            std::string sse_err;
                            if (Json::parseFromStream(rdr, sse_in, &sse_json, &sse_err)) {
                                std::string token;
                                auto is_progress = sse_json.get("_progress", false).asBool()
                                    || sse_json.get("_chunk_type", "").asString() == "progress";
                                auto is_llm_chunk = sse_json.get("_chunk_type", "").asString() == "llm_chunk";
                                auto source = sse_json.get("_source", "").asString();
                                auto skill = sse_json.get("_skill", "").asString();
                                // 只处理 coordinator 的消息：_source="" 且 _skill="" 或 "coordinator"
                                // 跳过 skill_executor 的消息（_skill="outfit-stylist" 等）
                                if (!skill.empty() && skill != "coordinator") continue;
                                auto delta = sse_json["choices"][0]["delta"];
                                if (delta.isObject()) token = delta.get("content", "").asString();
                                if (token.empty()) {
                                    auto msg = sse_json["choices"][0]["message"];
                                    if (msg.isObject()) token = msg.get("content", "").asString();
                                }
                                if (!token.empty()) {
                                    // llm_chunk: coordinator 的流式 token 作为 progress 推给上游，
                                    // 仅 tool_calls-only（无文本）的 llm_chunk 跳过。
                                    if (is_llm_chunk && token[0] == '{') continue;
                                    // progress/text 的 content 可能嵌套了 raw OpenAI JSON
                                    //（coordinator 节点写入 databus 时未剥壳），递归提取纯文本。
                                    // 检查内容中是否嵌入 JSON（不以 { 开头但包含 { 的场景，
                                    // 如 "coordinator 决策中…{"choices":[...]}"）
                                    auto json_start = token.find('{');
                                    if (json_start != std::string::npos) {
                                        std::string json_part = token.substr(json_start);
                                        Json::Value nested;
                                        Json::CharReaderBuilder nested_rdr;
                                        std::istringstream nested_in(json_part);
                                        std::string nested_err;
                                        if (Json::parseFromStream(nested_rdr, nested_in, &nested, &nested_err)) {
                                            auto nested_msg = nested["choices"][0]["message"];
                                            if (nested_msg.isObject()) {
                                                std::string t = nested_msg.get("content", "").asString();
                                                if (!t.empty()) {
                                                    token = token.substr(0, json_start) + t;
                                                }
                                            }
                                            if (token.empty() || token == json_part) {
                                                auto nested_delta = nested["choices"][0]["delta"];
                                                if (nested_delta.isObject()) {
                                                    std::string t = nested_delta.get("content", "").asString();
                                                    if (!t.empty()) {
                                                        token = token.substr(0, json_start) + t;
                                                    }
                                                }
                                            }
                                        }
                                        // 如果提取后仍然是纯 JSON（delta.content 为空，仅含 tool_calls），
                                        // 说明没有用户可见文本，跳过这个 chunk。
                                        if (!token.empty() && token[0] == '{') {
                                            token.clear();
                                        }
                                    }
                                    // 仅 _progress=true 的消息作为思考过程（flag=1），
                                    // 其余（llm_chunk、text 等）均为回复内容（flag=0）。
                                    // 最终完整回复（text）不累积，避免与前面的 llm_chunk 重复。
                                    int flag = sse_json.get("_progress", false).asBool() ? 1 : 0;
                                    // text chunk（非 llm_chunk、非 progress）是 coordinator 的最后完整回复。
                                    // llm_chunk 的增量 token 已完整覆盖回复内容，跳过 text 回调，
                                    // 避免前端收到流式 token + 最终回复的双份重复内容。
                                    if (!is_llm_chunk && !is_progress) {
                                        continue;
                                    }
                                    accumulated += token;
                                    has_content = has_content || !is_progress;
                                    // 只发增量（delta）
                                    std::string delta = accumulated.substr(last_sent.size());
                                    last_sent = accumulated;
                                    // 空增量不触发 Running 回调（tool_calls-only chunk 等无文本场景），
                                    // 但 Completed 收尾回调不受影响（见下方 SSE 流结束处的独立调用）。
                                    if (!delta.empty()) {
                                        // UserOutputFlag: 0=用户可见文本, 1=progress
                                        post_callback("Running", delta, flag);
                                    }
                                }
                            }
                        }
                    }
                    return true;
                });

            // SSE 流结束（Post 返回后）
            if (has_content) {
                std::string final_delta = accumulated.substr(last_sent.size());
                // 只发尚未通过 Running 发送的剩余内容，不重复发已经发过的
                post_callback("Completed", final_delta);
            }
            // TaskStatus 报文
            Json::Value ts;
            ts["Header"]["MessageType"] = "TaskStatusManagement";
            ts["Header"]["TaskID"] = request_id;
            ts["Header"]["Sender"] = "SysAgent";
            ts["Header"]["Receiver"] = "APP";
            ts["Header"]["SeqNum"] = request_id;
            ts["Header"]["UserName"] = user_name;
            ts["Header"]["ClientSessionId"] = client_session_id;
            ts["Data"]["MessageType"] = "TaskStatusReport";
            ts["Data"]["TaskStatusCode"] = has_content ? 200 : 500;
            ts["Data"]["TaskStatusReport"] = has_content ? "TaskCompleted" : "TaskFailed";
            ts["Data"]["UserOutputText"] = accumulated;
            httplib::Client ts_cli(sys_agent_entity.c_str());
            ts_cli.Post("/coremind/systemagent/v1/callback",
                        {{"Content-Type", "application/json"}},
                        Json::writeString(json_writer(), ts), "application/json");

            std::cerr << "[coremind_bridge] ASYNC_DONE req=" << request_id
                      << " accumulated=" << accumulated.size() << " chars" << std::endl;
        }).detach();
        return;
}

struct ListenAddress {
    std::string host;
    int port;
};

static ListenAddress parse_listen_address(const std::string& value) {
    const auto separator = value.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) {
        throw std::invalid_argument("listen address must use <host>:<port>");
    }

    const auto port = std::stoi(value.substr(separator + 1));
    if (port < 1 || port > 65535) {
        throw std::out_of_range("listen port must be between 1 and 65535");
    }
    return {value.substr(0, separator), port};
}

// ── CoreMind 框架初始化（仅生产模式启用） ──
#ifdef COREMIND_ENABLED
static bool init_coremind_framework(int argc, char* argv[]) {
    // 1. 创建 Router 并注册路由
    g_sidecar_router = std::make_shared<SidecarRouter>();

    // 注册路由表 —— 对标 HealthAgent SysCallBack::RegisterRouter()
    // 上游通过 CoreMind 框架的 HTTP/2 + SSL 访问这些端点
    const std::string kUserRequestUrl = "/coremind/systemagent/v1/callback";
    const std::string kHealthCheckUrl = "/healthcheck";
    const std::string kScheduleTaskAbilityUrl = "/sysagent/v1/scheduleTaskAbility";

    g_sidecar_router->Register(
        {CMFrm::COM::GET,  kHealthCheckUrl,          CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleHealthCheck});
    g_sidecar_router->Register(
        {CMFrm::COM::GET,  kScheduleTaskAbilityUrl,   CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleScheduleTaskAbility});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, kUserRequestUrl,            CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleUserRequest});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/health/v1/tasks",         CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleUserRequest});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/aisf/outfit/v1/tasks",    CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleUserRequest});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/aisf/sysagent/v1/agentInstall",   CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleAgentInstall});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/aisf/sysagent/v1/agentUpdate",    CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleAgentUpdate});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/aisf/sysagent/v1/agentUninstall", CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleAgentUninstall});

    // 2. 初始化 CoreMind 框架
    CMFrm::Starter::FrameworkInitParam frameworkInitParam;
    frameworkInitParam.SetProcessArgs(argc, argv);
    frameworkInitParam.AddServiceRouter(g_sidecar_router);

    CMFrm::Starter::FrameworkStarter::InitFramework(frameworkInitParam);

    std::cout << "[coremind_bridge] CoreMind framework initialized"
              << " — listening on CoreMind HTTP/2 + SSL" << std::endl;

    // 注册 HTTP/2 客户端配置（对标 HealthAgent InitUMOMFrm）
    // UDS LLM 代理生产路径通过此客户端以 HTTPS + mTLS 调 IMInferScheduler
    {
        auto httpFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
        auto streamConfig = std::make_shared<CMFrm::COM::Http2ClientConfig>();
        streamConfig->name = "default";
        streamConfig->timeoutSecond = 600;
        httpFactory->RegisterHttp2ClientConfig(streamConfig);
    }

    // 预缓存服务实例（对标 HealthAgent InitUMOMFrm）
    // 避免首次请求时额外的服务发现延迟
    std::cout << "[coremind_bridge] Pre-caching CoreMind service instances..." << std::endl;
    auto required_services = split_services(std::getenv("COREMIND_REQUIRED_SERVICES"));
    auto optional_services = split_services(std::getenv("COREMIND_OPTIONAL_SERVICES"));
    for (const auto& name : required_services) {
        auto snap = service_snapshot(name);
        if (snap["status"].asString() == "resolved") {
            std::cout << "  [precache] " << name << " resolved" << std::endl;
        } else {
            std::cout << "  [precache] " << name << " UNAVAILABLE (" << snap["error_code"].asString() << ")" << std::endl;
        }
    }
    for (const auto& name : optional_services) {
        auto snap = service_snapshot(name);
        if (snap["status"].asString() == "resolved") {
            std::cout << "  [precache] " << name << " resolved (optional)" << std::endl;
        }
    }
    std::cout << "[coremind_bridge] Pre-cache complete" << std::endl;
    return true;
}
#endif

int main(int argc, char* argv[]) {
    std::string listen_addr = "0.0.0.0:23767";
    std::string bridge_url = "http://127.0.0.1:8199";
    const char* env_uds = std::getenv("AF_PLUGIN_UDS_FILE");
    std::string uds_socket = env_uds ? env_uds : "";
    std::string llm_api_url;
    bool coremind_mode = false;

    // ── CLI 参数解析 ──
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            listen_addr = argv[++i];
        } else if (arg == "--bridge-url" && i + 1 < argc) {
            bridge_url = argv[++i];
        } else if (arg == "--uds-socket" && i + 1 < argc) {
            uds_socket = argv[++i];
        } else if (arg == "--llm-api-url" && i + 1 < argc) {
            llm_api_url = argv[++i];
        } else if (arg == "--coremind-mode") {
            coremind_mode = true;
        } else if (arg == "--process-type" && i + 1 < argc) {
            // 消费参数但不处理 —— FrameworkStarter::InitFramework 会从 argv 解析它
            // 用于 CoreMind 服务注册的服务名标识（如 OutfitAgent）
            i++;
        } else {
            std::cerr << "Usage: coremind_bridge"
                      << " --listen <ip:port>"
                      << " --bridge-url <url>"
                      << " [--uds-socket <path>]"
                      << " [--llm-api-url <url>]"
                      << " [--coremind-mode]"
                      << std::endl;
            return 1;
        }
    }

    // 如果指定了 UDS socket 但没有 LLM API URL，从环境变量读取
    // CoreMind 模式下不需要 --llm-api-url（UDS 代理通过服务发现访问 IMInferScheduler）
    if (!coremind_mode && !uds_socket.empty() && llm_api_url.empty()) {
        const char* env_url = std::getenv("AF_LLM_BASE_URL");
        if (env_url != nullptr) {
            llm_api_url = env_url;
        } else {
            std::cerr << "[coremind_bridge] --llm-api-url or AF_LLM_BASE_URL required when using --uds-socket"
                      << std::endl;
            std::cerr << "  (not needed with --coremind-mode, where IMInferScheduler is resolved via service discovery)"
                      << std::endl;
            return 1;
        }
    }

    // ═══════════════════════════════════════════════════════════════
    //  模式 A: CoreMind 框架模式 — 通过 ServiceRouter 接收 HTTPS HTTP/2
    // ═══════════════════════════════════════════════════════════════
#ifdef COREMIND_ENABLED
    if (coremind_mode) {
        std::signal(SIGTERM, signal_handler);
        std::signal(SIGINT, signal_handler);

        // UDS 模型代理 — CoreMind 模式走 IMInferScheduler 生产路径
        UdsModelProxy uds_proxy;
        if (!uds_socket.empty()) {
            if (!uds_proxy.start(uds_socket, llm_api_url, true)) {
                std::cerr << "[coremind_bridge] FAILED to start UDS proxy on "
                          << uds_socket << std::endl;
                return 1;
            }
        }

        if (!init_coremind_framework(argc, argv)) {
            std::cerr << "[coremind_bridge] FAILED to initialize CoreMind framework" << std::endl;
            return 1;
        }

        // 阻塞等待 CoreMind 框架退出信号
        CMFrm::Starter::FrameworkStarter::WaitProcessExit();

        std::cout << "[coremind_bridge] CoreMind framework stopped" << std::endl;
        return 0;
    }
#endif

    // ═══════════════════════════════════════════════════════════════
    //  模式 B: 标准 HTTP/1.1 模式（httplib）— 用于本地开发 / stub 测试
    // ═══════════════════════════════════════════════════════════════
    ListenAddress listen;
    try {
        listen = parse_listen_address(listen_addr);
    } catch (const std::exception& error) {
        std::cerr << "[coremind_bridge] invalid --listen value: " << error.what() << std::endl;
        return 1;
    }

    httplib::Server svr;
    g_http_server.store(&svr);

    // 注册信号处理，支持 graceful shutdown
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    configure_server(svr, bridge_url);

    // UDS 模型代理
    UdsModelProxy uds_proxy;
    if (!uds_socket.empty()) {
        if (!uds_proxy.start(uds_socket, llm_api_url)) {
            std::cerr << "[coremind_bridge] FAILED to start UDS proxy on " << uds_socket << std::endl;
            return 1;
        }
    }

    std::cout << "[coremind_bridge] listening on " << listen_addr << std::endl;
    std::cout << "[coremind_bridge] forwarding to bridge: " << bridge_url << std::endl;

    if (!svr.bind_to_port(listen.host, listen.port)) {
        std::cerr << "[coremind_bridge] FAILED to bind " << listen_addr << std::endl;
        g_http_server.store(nullptr);
        return 1;
    }
    svr.listen_after_bind();

    std::cout << "[coremind_bridge] server stopped" << std::endl;
    g_http_server.store(nullptr);
    return 0;
}

static void configure_server(httplib::Server& svr, const std::string& bridge_url) {

    // ═══════════════════════════════════════════════
    //  1. Health check (handled locally)
    // ═══════════════════════════════════════════════
    const auto health_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path << std::endl;
        Json::Value json;
        json["status"] = "healthy";
        json["coremind"] = true;
        json["sidecar"] = true;
        set_json_response(res, json);
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 200 | body={\"status\":\"healthy\"}" << std::endl;
    };
    svr.Get("/health", health_handler);
    svr.Get("/healthcheck", health_handler);

    // ═══════════════════════════════════════════════
    //  2. Capability advertisement (handled locally)
    // ═══════════════════════════════════════════════
    svr.Get("/sysagent/v1/scheduleTaskAbility",
            [](const httplib::Request& req, httplib::Response& res) {
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path << std::endl;
        Json::Value json;
        json["agent_name"] = "outfit-assistant";
        json["version"] = "1.0.0";
        json["capabilities"] = Json::Value(Json::arrayValue);
        json["capabilities"].append("outfit_consult");
        json["capabilities"].append("weather_query");
        json["capabilities"].append("occasion_guide");
        json["capabilities"].append("image_generation");
        json["capabilities"].append("video_generation");
        json["endpoint"] = "/coremind/systemagent/v1/callback";
        json["async_mode"] = true;
        set_json_response(res, json);
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 200 | body={\"agent_name\":\"outfit-assistant\"}" << std::endl;
    });

    // ═══════════════════════════════════════════════
    //  3. Task callback → discover services and forward to Python Bridge
    // ═══════════════════════════════════════════════
    // Matches HealthAgent's USER_REQUEST_URL
    svr.Post("/coremind/systemagent/v1/callback",
             [bridge_url](const httplib::Request& req, httplib::Response& res) {
        forward_to_bridge(bridge_url, req, res);
    });

    // Alternate task entry paths (forward to adapter too)
    svr.Post("/health/v1/tasks",
             [bridge_url](const httplib::Request& req, httplib::Response& res) {
        forward_to_bridge(bridge_url, req, res);
    });

    svr.Post("/aisf/outfit/v1/tasks",
             [bridge_url](const httplib::Request& req, httplib::Response& res) {
        forward_to_bridge(bridge_url, req, res);
    });

    // ═══════════════════════════════════════════════
    //  4. Service discovery (handled locally via CoreMind)
    // ═══════════════════════════════════════════════
    svr.Get("/resolve/:name", [](const httplib::Request& req, httplib::Response& res) {
        auto name = req.path_params.at("name");
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path << " (name=" << name << ")" << std::endl;
        auto result = resolve_service(name);

        Json::Value json;
        json["service"] = name;
        json["status"] = result.success ? "ok" : "error";
        if (result.success) {
            json["instances"] = Json::Value(Json::arrayValue);
            for (const auto& inst : result.instances) {
                Json::Value item;
                item["ip"] = inst.ip;
                item["port"] = inst.port;
                json["instances"].append(item);
            }
        } else {
            json["error"] = result.error;
        }
        int status = result.success ? 200 : 503;
        set_json_response(res, json, status);
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " " << status << " | body={\"status\":\"" << json["status"].asString() << "\"}" << std::endl;
    });

    svr.Post("/resolve/batch", [](const httplib::Request& req, httplib::Response& res) {
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path << std::endl;

        Json::CharReaderBuilder reader_builder;
        Json::Value request_json;
        std::string errors;
        std::istringstream input(req.body);
        if (!Json::parseFromStream(reader_builder, input, &request_json, &errors) ||
            !request_json["services"].isArray()) {
            res.status = 400;
            res.set_content(R"({"status":"error","error":"services must be an array"})",
                            "application/json");
            std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                      << " 400 | body={\"status\":\"error\"}" << std::endl;
            return;
        }

        Json::Value response;
        response["status"] = "ok";
        response["services"] = Json::Value(Json::objectValue);
        bool all_resolved = true;
        for (const auto& service : request_json["services"]) {
            if (!service.isString() || service.asString().empty()) {
                all_resolved = false;
                continue;
            }
            const auto name = service.asString();
            const auto result = resolve_service(name);
            Json::Value item;
            item["status"] = result.success ? "ok" : "error";
            item["instances"] = Json::Value(Json::arrayValue);
            for (const auto& instance : result.instances) {
                Json::Value endpoint;
                endpoint["ip"] = instance.ip;
                endpoint["port"] = instance.port;
                item["instances"].append(endpoint);
            }
            if (!result.success) {
                item["error"] = result.error;
                all_resolved = false;
            }
            response["services"][name] = item;
        }
        if (!all_resolved) {
            response["status"] = "partial";
        }
        set_json_response(res, response);
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 200 | body={\"status\":\"" << response["status"].asString() << "\"}" << std::endl;
    });

    // ═══════════════════════════════════════════════
    //  5. CoreMind 平台服务代理（Bridge 通过此接口调用系统公共能力）
    // ═══════════════════════════════════════════════
    svr.Post("/proxy/memory/query", handle_proxy_memory_query);
    svr.Post("/proxy/memory/store", handle_proxy_memory_store);
    svr.Post("/proxy/rag/search", handle_proxy_rag_search);
    svr.Post("/proxy/context/add", handle_proxy_context_add);

    // ═══════════════════════════════════════════════
    //  6. Agent management stubs (handled locally)
    // ═══════════════════════════════════════════════
    svr.Post("/aisf/sysagent/v1/agentInstall",
             [](const httplib::Request& req, httplib::Response& res) {
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path << std::endl;
        res.set_content(R"({"status":"ok","action":"install"})", "application/json");
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 200 | body={\"status\":\"ok\",\"action\":\"install\"}" << std::endl;
    });

    svr.Post("/aisf/sysagent/v1/agentUpdate",
             [](const httplib::Request& req, httplib::Response& res) {
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path << std::endl;
        res.set_content(R"({"status":"ok","action":"update"})", "application/json");
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 200 | body={\"status\":\"ok\",\"action\":\"update\"}" << std::endl;
    });

    svr.Post("/aisf/sysagent/v1/agentUninstall",
             [](const httplib::Request& req, httplib::Response& res) {
        std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path << std::endl;
        res.set_content(R"({"status":"ok","action":"uninstall"})", "application/json");
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 200 | body={\"status\":\"ok\",\"action\":\"uninstall\"}" << std::endl;
    });

    // ═══════════════════════════════════════════════
    //  6. 404 catch-all
    // ═══════════════════════════════════════════════
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (res.status == 404) {
            Json::Value json;
            json["error"] = "not found";
            json["path"] = req.path;
            set_json_response(res, json, 404);
        }
    });

}
