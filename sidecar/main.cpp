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
#include <chrono>
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
#include "router_logic.h"

// CoreMind framework headers — only available in Huawei build environment
#ifdef COREMIND_ENABLED
#include <framework_com/http_client_factory.h>
#include <framework_starter/framework_starter.h>
#include <mm_sdk.h>
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

static std::string current_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(ms);
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

    // B1: EventType 校验 — 与生产 HandleUserRequest 一致，非法组合 400 拒绝
    std::string header_msg_type = header.get("MessageType", "").asString();
    std::string data_msg_type = data.get("MessageType", "").asString();
    if (!sidecar::is_supported_message_type(header_msg_type, data_msg_type)) {
        res.status = 400;
        res.set_content(
            R"({"error":"unsupported message type: )" + header_msg_type + "/" + data_msg_type + R"("})",
            "application/json");
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 400 | body={\"error\":\"unsupported message type\"}" << std::endl;
        return;
    }

    std::string user_name = header.get("UserName", "").asString();
    std::string client_session_id = header.get("ClientSessionId",
                               header.get("SessionId", "")).asString();
    std::string task_id = header.get("TaskID", "").asString();
    std::string dialogue_id = header.get("DialogueId", "").asString();
    std::string sender = header.get("Sender", "").asString();
    std::string receiver = header.get("Receiver", "").asString();
    std::string user_input_text = data.get("UserInputText", "").asString();
    std::string user_input_object_str = data.get("UserInputObject", "").asString();
    std::string sys_agent_entity = header.get("SysAgentEntity", "").asString();

    // B10: 构建 OpenAI 多模态 content — UserInputObject 是 stringified JSON string（CMF 协议），
    // 与生产 HandleUserRequest 共用 build_openai_messages（嵌套对象会被 asString() 丢弃图片）。
    auto openai_messages = sidecar::build_openai_messages(user_input_text, user_input_object_str);
    Json::Value content_parts = openai_messages[0]["content"];
    // content 直接传 ContentPart 数组（JSON 数组，非字符串化）
    // bridge 的 normalize_content 需要数组才能识别 image_url part 并落盘；
    // 纯文本场景传 [{type:text,text:...}] 数组，bridge prepare_content_for_daemon
    // 同样返回纯文本，行为一致。

    // 构造 REST API 请求体
    static std::atomic<unsigned long long> request_counter{0};
    auto request_id = req.get_header_value("X-Request-Id");
    if (request_id.empty()) {
        request_id = "sidecar-" + std::to_string(++request_counter);
    }

    Json::Value rest_body;
    rest_body["user_id"] = user_name;
    rest_body["client_id"] = client_session_id;
    rest_body["content"] = content_parts;

    // 从 bridge_url 提取 origin 和 path prefix
    const auto target = parse_bridge_target(bridge_url);
    const char* env_agent = std::getenv("AF_BR_COREMIND_AGENT_ID");
    std::string agent_id = env_agent ? env_agent : "outfit-assistant";

    // ══════════════════════════════════════════════════════════════
    //  有 SysAgentEntity → 异步模式：200 无 body + 后台 SSE 流式转发 + POST 回调
    //  无 SysAgentEntity → 同步模式：httplib POST ?stream=false（对齐生产同步分支）
    // ══════════════════════════════════════════════════════════════
    if (!sys_agent_entity.empty()) {
        // 对齐生产 HandleUserRequest：异步路径只返回 200 状态码（WriteStatueCode(200) 无 body）
        res.status = 200;
        std::cerr << "[coremind_bridge] ASYNC " << req.path << " req=" << request_id
                  << " callback=" << sys_agent_entity << std::endl;

        // 后台线程处理 SSE 流 + POST 回调
        std::thread([=]() {
            std::string stream_path = target.path_prefix + "/api/chat/" + agent_id + "?stream=true";
            httplib::Client cli(target.origin.c_str());
            cli.set_read_timeout(360, 0);
            httplib::Headers hdrs;
            hdrs.emplace("Content-Type", "application/json");
            hdrs.emplace("X-Request-Id", request_id);

            std::string sse_buffer;
            sidecar::SSEChunkProcessor processor;
            auto dumper = std::make_shared<sidecar::SidecarDumper>(request_id);

            // POST 回调辅助函数 — 对齐生产 sidecar_router.cpp 的回调构造：
            //   Data 用 build_user_output_data（含 UserOutputObject/IsVoiceBroadcast/
            //   ImplicitOutputFlag/UserManualInterventionFlag），UserOutputObject 覆盖为
            //   stringified artifacts（Completed）或空串（Running）；
            //   Header 用 build_callback_header（TaskID=原始 task_id、DialogueId、
            //   Sender/Receiver 翻转、透传 AcmSessionId/ClientExField/DeviceCategory/
            //   DeviceId/SessionId/TeamEnvironmentID/UUID、SeqNum=request_id）+ EventTimeStamp。
            //   回调 URL：dev 模式 http://{SysAgentEntity}（本地 mock 无 TLS）；
            //   生产模式 https://{SysAgentEntity}:23767（CoreMind HTTP/2 + mTLS）——环境差异。
            auto post_callback = [&](const std::string& stream_status, int flag,
                                       const std::string& text, bool is_final,
                                       const std::string& user_output_object) -> bool {
                dumper->dump_callback(stream_status, flag, text, is_final, user_output_object);
                Json::Value cb;
                cb["Data"] = sidecar::build_user_output_data(stream_status, flag, text);
                cb["Data"]["UserOutputObject"] = user_output_object;
                cb["Header"] = sidecar::build_callback_header(
                    "DataExchange", task_id, dialogue_id, user_name, client_session_id,
                    sender, receiver, request_id, header);
                cb["Header"]["EventTimeStamp"] = sidecar::current_timestamp_iso8601();
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
                return true;
            };

            std::string body_str = Json::writeString(json_writer(), rest_body);
            dumper->dump_request(rest_body);
            std::cerr << "[coremind_bridge] FWD " << stream_path << " req=" << request_id
                      << " | body=\n" << pretty_json(body_str) << std::endl;
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
                            std::cerr << "[coremind_bridge] SSE_RAW len=" << payload.size()
                                      << " data=" << payload << std::endl;
                            Json::Value sse_json;
                            Json::CharReaderBuilder rdr;
                            std::istringstream sse_in(payload);
                            std::string sse_err;
                            if (Json::parseFromStream(rdr, sse_in, &sse_json, &sse_err)) {
                                dumper->dump_sse_chunk(sse_json);
                                for (const auto& cb : processor.process(sse_json)) {
                                    post_callback(cb.stream_status, cb.flag, cb.text,
                                                  cb.is_final, cb.user_output_object);
                                }
                            }
                        }
                    }
                    return true;
                });

            // ── 任务结束：无论 bridge 成功/失败/超时都必须通知 upstream ──
            // 错误分流（对齐生产 sidecar_router.cpp + bridge chat_rest.py 错误两路）：
            //   ① HTTP 不可达/断开/超时 → ts=500/BridgeError，error=httplib error
            //   ② HTTP 非 200          → ts=resp.status/BridgeError，body 含 error detail
            //   ③ HTTP 200 但 SSE 流内 error payload → ts=500/BridgeError，error_message_ 携详情
            //   ④ HTTP 200 且无错误    → ts=200/TaskCompleted，先发 Completed UserOutput 再发 TaskStatus
            // 错误时跳过 Completed 回调（避免上游收到虚假"完成"信号），仅发错误 TaskStatus。
            // 防止 upstream 永久等待，也避免 bridge 故障时上报虚假 200/TaskCompleted（F1 数据安全）。
            int ts_code;
            std::string ts_status;
            std::string ts_error_detail;  // 提取的 bridge 错误详情，拼到 TaskStatusReport 让上游可定位
            if (!result) {
                ts_code = 500;
                ts_status = "BridgeError";
                ts_error_detail = httplib::to_string(result.error());
                std::cerr << "[coremind_bridge] BRIDGE_ERR req=" << request_id
                          << " error=" << ts_error_detail << std::endl;
            } else if (result->status != 200) {
                ts_code = result->status;
                ts_status = "BridgeError";
                ts_error_detail = result->body.substr(0, 500);
                std::cerr << "[coremind_bridge] BRIDGE_HTTP_ERR req=" << request_id
                          << " http=" << result->status
                          << " body=" << ts_error_detail << std::endl;
            } else if (processor.has_error()) {
                // bridge SSE 流内 error payload（chat_rest.py:1004-1021 daemon 通知中途失败/timeout）
                ts_code = 500;
                ts_status = "BridgeError";
                ts_error_detail = processor.error_message();
                std::cerr << "[coremind_bridge] SSE_STREAM_ERR req=" << request_id
                          << " error=" << ts_error_detail << std::endl;
            } else {
                ts_code = 200;
                ts_status = "TaskCompleted";
                std::cerr << "[coremind_bridge] STREAM_DONE req=" << request_id
                          << " content_len=" << processor.accumulated_content().size() << std::endl;
            }

            // 仅 bridge 完全成功时发 Completed UserOutput 回调；失败时上游只收 TaskStatus（对齐生产）
            if (ts_code == 200) {
                auto completed = processor.make_completed();
                post_callback(completed.stream_status, completed.flag, completed.text,
                              completed.is_final, completed.user_output_object);
            }

            // TaskStatusReport（成功/失败都发，防止 upstream 永久等待）
            Json::Value task_status;
            // 错误时把 bridge 详情拼到 UserOutputText，上游可据此定位（F7 错误可定位 + F1 禁止掩盖）
            // 成功时仍是 coordinator 的最终回答 accumulated_content。
            std::string ts_text = (ts_code == 200)
                ? processor.accumulated_content()
                : ("bridge_error: " + ts_error_detail);
            task_status["Data"] = sidecar::build_task_status_data(ts_code, ts_status, ts_text);
            task_status["Header"] = sidecar::build_callback_header(
                "TaskStatusManagement", task_id, dialogue_id, user_name, client_session_id,
                sender, receiver, request_id, header);
            // EventTimeStamp 必须用 ISO 8601 格式（sidecar_interface_contract.md §3 报文 +
            // router_logic.h:72-73 呑约），与 UserOutput 回调（L278）和生产路径保持对称。
            task_status["Header"]["EventTimeStamp"] = sidecar::current_timestamp_iso8601();
            std::string ts_str = Json::writeString(json_writer(), task_status);
            httplib::Client ts_cli(sys_agent_entity.c_str());
            ts_cli.Post("/coremind/systemagent/v1/callback",
                        {{"Content-Type", "application/json"}},
                        ts_str, "application/json");

            std::cerr << "[coremind_bridge] ASYNC_DONE req=" << request_id
                      << " ts_code=" << ts_code << " ts_status=" << ts_status
                      << " accumulated=" << processor.accumulated_content().size() << " chars" << std::endl;
        }).detach();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    //  同步模式（无 SysAgentEntity）— 对齐生产 HandleUserRequest 同步分支：
    //  httplib POST ?stream=false，返回 200（有内容）/ 500（Bridge 不可达）
    // ══════════════════════════════════════════════════════════════
    std::string sync_path = target.path_prefix + "/api/chat/" + agent_id + "?stream=false";
    httplib::Client sync_cli(target.origin.c_str());
    sync_cli.set_read_timeout(360, 0);
    httplib::Headers sync_headers;
    sync_headers.emplace("Content-Type", "application/json");
    sync_headers.emplace("X-Request-Id", request_id);

    std::string sync_body = Json::writeString(json_writer(), rest_body);
    std::cerr << "[coremind_bridge] SYNC_FWD " << sync_path << " req=" << request_id
              << " | body=\n" << pretty_json(sync_body) << std::endl;
    auto sync_response = sync_cli.Post(sync_path.c_str(), sync_headers, sync_body, "application/json");
    if (!sync_response) {
        res.status = 500;
        std::cerr << "[coremind_bridge] SYNC_ERR req=" << request_id
                  << " upstream unreachable" << std::endl;
        return;
    }

    std::string sync_text;
    if (!sync_response->body.empty()) {
        Json::Value bridge_resp;
        Json::CharReaderBuilder sync_reader;
        std::string sync_errors;
        std::istringstream sync_input(sync_response->body);
        if (Json::parseFromStream(sync_reader, sync_input, &bridge_resp, &sync_errors)) {
            sync_text = bridge_resp.get("content", "").asString();
        } else {
            sync_text = sync_response->body;
        }
    }

    std::string sync_preview = sync_text.substr(0, 500);
    if (sync_text.size() > 500) sync_preview += "... (truncated)";
    std::cout << "[coremind_bridge] sync response: request_id=" << request_id
              << " resp_len=" << sync_text.size()
              << " content=" << sync_preview << std::endl;

    res.status = 200;
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
        {CMFrm::COM::GET,  "/health",                 CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleHealthCheck});
    g_sidecar_router->Register(
        {CMFrm::COM::GET,  kScheduleTaskAbilityUrl,   CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleScheduleTaskAbility});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, kUserRequestUrl,            CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleUserRequest});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/health/v1/tasks",         CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleUserRequest});
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/aisf/outfit/v1/tasks",    CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleUserRequest});
    // 与 deployment/conf/agents.json 中 OutfitAgent 的 baseUrl 契约保持一致
    // （上游 CSP 平台按 baseUrl=/aisf/outfitagent/v1/tasks 下发任务）
    g_sidecar_router->Register(
        {CMFrm::COM::POST, "/aisf/outfitagent/v1/tasks", CMFrm::COM::RAW, "default", -1, SidecarRouter::HandleUserRequest});
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

// ── MateMind SDK 初始化（对标 SysAgent initMateMindSdk） ──
// MM_SdkInit 读取 /opt/coremind/conf/matemindsdk.json，其中 callbackserverport=27171
// 该端口注册为 CMF_LABEL_MAP_mmsdk-callback-server，使框架服务注册包含 27171 端口
static bool init_mate_mind_sdk() {
    auto log_func = [](mmsdk::MMlogLevel level, const std::string& str) -> uint32_t {
        switch (level) {
            case mmsdk::MM_LOG_DEBUG:
                std::clog << "[matemindsdk][D] " << str << std::endl;
                break;
            case mmsdk::MM_LOG_INFO:
                std::clog << "[matemindsdk][I] " << str << std::endl;
                break;
            case mmsdk::MM_LOG_WARN:
                std::clog << "[matemindsdk][W] " << str << std::endl;
                break;
            case mmsdk::MM_LOG_ERR:
            case mmsdk::MM_LOG_FATAL:
                std::cerr << "[matemindsdk][E] " << str << std::endl;
                break;
            default:
                break;
        }
        return 0;
    };

    constexpr const char* kConfigPath = "/opt/coremind/conf/matemindsdk.json";
    if (mmsdk::MM_SdkInit(kConfigPath, log_func) != 0) {
        std::cerr << "[coremind_bridge] MateMind SDK init failed (config: " << kConfigPath << ")"
                  << std::endl;
        return false;
    }
    std::cout << "[coremind_bridge] MateMind SDK initialized (callback port 27171)" << std::endl;
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
        } else if (arg == "-processType" && i + 1 < argc) {
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
        // 把 --bridge-url 注入到 SidecarRouter 的 static 配置。
        // 必须在 init_coremind_framework 前调用——framework init 会注册 SidecarRouter 的
        // URL patterns，之后框架回调 HandleUserRequest 时会读 bridge_url_。
        // 不注入的话 SidecarRouter 用 class 默认值 http://127.0.0.1:8199（不含 /health），
        // 导致转发 path 缺 /health 前缀，bridge 新架构 404。
        SidecarRouter::set_bridge_url(bridge_url);

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

        if (!init_mate_mind_sdk()) {
            std::cerr << "[coremind_bridge] FAILED to initialize MateMind SDK" << std::endl;
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
    // 与 deployment/conf/agents.json 中 OutfitAgent 的 baseUrl 契约保持一致
    svr.Post("/aisf/outfitagent/v1/tasks",
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
