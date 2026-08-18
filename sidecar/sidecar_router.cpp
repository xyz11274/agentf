#include "sidecar_router.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <atomic>
#include <vector>

#include <json/json.h>
#include <httplib.h>
#include <framework_com/http_client_factory.h>

#include "resolve_handler.h"
#include "router_logic.h"
#include "acm_client.h"  // AcmClient::fetch_url / upload_file（产物 ACM 上传，L4 §4.2）

using sidecar::build_callback_header;
using sidecar::current_timestamp_iso8601;
using sidecar::build_health_response;
using sidecar::build_user_output_object;
using sidecar::parse_request_body;

// ── SidecarRouter static 成员定义 ──
// 声明在 sidecar_router.h，这里定义并初始化默认值。
// 默认值与原 get_env_or("BRIDGE_URL", "http://127.0.0.1:8199") 一致。
// main.cpp 解析 --bridge-url 后通过 set_bridge_url() 覆盖。
std::string SidecarRouter::bridge_url_ = "http://127.0.0.1:8199";
// agent 路由配置默认值：与旧硬编码 outfit-assistant 行为一致，等待 main.cpp 注入覆盖。
std::string SidecarRouter::agent_name_ = "outfit-assistant";
std::string SidecarRouter::agent_version_ = "1.0.0";
std::vector<std::string> SidecarRouter::agent_capabilities_ = {
    "outfit_consult", "weather_query", "occasion_guide",
    "image_generation", "video_generation"};

// 配置注入：main.cpp 加载 sidecar.json 后调用（在 framework init 前）。
void SidecarRouter::set_agent_config(const std::string& name, const std::string& version,
                                     const std::vector<std::string>& capabilities) {
    agent_name_ = name;
    agent_version_ = version;
    agent_capabilities_ = capabilities;
}

// ── JSON 辅助 ──

static std::string current_timestamp_ms() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms);
}

static Json::StreamWriterBuilder json_writer() {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    return w;
}

static std::string json_dumps(const Json::Value& v) {
    return Json::writeString(json_writer(), v);
}

static std::string pretty_json(const std::string& raw) {
    Json::CharReaderBuilder reader;
    Json::Value val;
    std::string errors;
    std::istringstream input(raw);
    if (Json::parseFromStream(reader, input, &val, &errors)) {
        // F7 展示层截断:base64 data URI 超长时截断,防止日志刷屏;存储层(dumper)保留完整
        return sidecar::json_to_styled_truncated(val);
    }
    return raw;
}

static std::vector<std::string> split_services(const char* value) {
    std::vector<std::string> result;
    if (value == nullptr) return result;
    std::istringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

static Json::Value service_snapshot(const std::string& name) {
    Json::Value snap;
    auto result = resolve_service(name);
    if (!result.success) {
        snap["status"] = "unavailable";
        snap["error_code"] = result.error;
        return snap;
    }
    snap["status"] = "resolved";
    snap["instances"] = Json::Value(Json::arrayValue);
    for (const auto& inst : result.instances) {
        Json::Value item;
        item["host"] = inst.ip;
        item["port"] = inst.port;
        snap["instances"].append(item);
    }
    return snap;
}

static std::string get_env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

// ── 响应辅助 ──
// 注意：CoreMind API 拼写为 WriteStatueCode（非 StatusCode），此乃框架自有拼写

static void ok_response(const std::shared_ptr<CMFrm::ServiceRouter::Context>& ctx,
                        const Json::Value& body) {
    ctx->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    ctx->WriteJSONContentType();
    ctx->WriteResponseBody(json_dumps(body));
}

static void error_response(const std::shared_ptr<CMFrm::ServiceRouter::Context>& ctx,
                           int status, const std::string& message) {
    ctx->WriteStatueCode(static_cast<CMFrm::COM::HttpStatus>(status));
    ctx->WriteJSONContentType();
    Json::Value err;
    err["error"] = message;
    ctx->WriteResponseBody(json_dumps(err));
}

// ═══════════════════════════════════════════════════════════════════
//  SidecarRouter 实现
// ═══════════════════════════════════════════════════════════════════

std::string SidecarRouter::GetAPIProvider() {
    return "coremind-bridge-sidecar";
}

bool SidecarRouter::Register(const CMFrm::ServiceRouter::UsrServiceRouter& router) {
    router_list_.push_back(router);
    return true;
}

std::vector<CMFrm::ServiceRouter::UsrServiceRouter> SidecarRouter::URLPatterns() {
    return router_list_;
}

// ═══════════════════════════════════════════════════════════════════
//  路由处理器
// ═══════════════════════════════════════════════════════════════════

CMFrm::COM::ServerRespHandleMode SidecarRouter::HandleHealthCheck(
    const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    Json::Value body = build_health_response();
    ok_response(context, body);
    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode SidecarRouter::HandleScheduleTaskAbility(
    const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    Json::Value body;
    body["agent_name"] = agent_name_;
    body["version"] = agent_version_;
    body["capabilities"] = Json::Value(Json::arrayValue);
    for (const auto& cap : agent_capabilities_) {
        body["capabilities"].append(cap);
    }
    body["endpoint"] = "/coremind/systemagent/v1/callback";
    body["async_mode"] = true;
    ok_response(context, body);
    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode SidecarRouter::HandleUserRequest(
    const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    // 1. 解析请求体
    std::string req_body = context->GetRequestBody();
    auto parsed = parse_request_body(req_body);
    if (!parsed.has_value()) {
        if (req_body.empty()) {
            error_response(context, 400, "empty request body");
        } else {
            error_response(context, 400, "invalid JSON");
        }
        return CMFrm::COM::SYNC;
    }
    Json::Value original_request = *parsed;

    // 3. 从 AccessCtrl 格式提取字段（全部保存用于响应回传）
    auto header = original_request["Header"];
    auto data = original_request["Data"];

    std::string header_msg_type = header.get("MessageType", "").asString();
    std::string data_msg_type = data.get("MessageType", "").asString();
    bool is_task_assignment = (header_msg_type == "UserTaskManagement" && data_msg_type == "TaskAssignment");
    bool is_user_input = (header_msg_type == "DataExchange" && data_msg_type == "UserInput");
    if (!is_task_assignment && !is_user_input) {
        error_response(context, 400,
            "unsupported message type: " + header_msg_type + "/" + data_msg_type);
        return CMFrm::COM::SYNC;
    }

    std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] REQ " << header_msg_type << "/" << data_msg_type
              << " | body=\n" << pretty_json(req_body) << std::endl;

    Json::Value req_header = header;
    std::string user_name = header.get("UserName", "").asString();
    std::string client_session_id = header.get("ClientSessionId",
                               header.get("SessionId", "")).asString();
    std::string task_id = header.get("TaskID", "").asString();
    std::string dialogue_id = header.get("DialogueId", "").asString();
    std::string sender = header.get("Sender", "").asString();
    std::string receiver = header.get("Receiver", "").asString();
    std::string user_input_text = data.get("UserInputText", "").asString();
    // 回调 SeqNum 透传上游请求 Header.SeqNum（上游发什么回什么），
    // 缺失时回退 request_id 保持非空（契约要求 SeqNum 必填）。
    std::string seq_num = header.get("SeqNum", "").asString();

    // 4. 构建多模态 content（OpenAI 格式）
    // UserInputObject 在 CMF 协议中是 stringified JSON string（对齐 SysAgent TaskAssignment::Parse）
    Json::Value content_parts(Json::arrayValue);
    if (!user_input_text.empty()) {
        Json::Value text_part;
        text_part["type"] = "text";
        text_part["text"] = user_input_text;
        content_parts.append(text_part);
    }
    std::string user_input_object_str = data.get("UserInputObject", "").asString();
    if (!user_input_object_str.empty()) {
        Json::Value user_input_obj;
        Json::CharReaderBuilder obj_reader;
        std::string obj_err;
        std::istringstream obj_stream(user_input_object_str);
        if (Json::parseFromStream(obj_reader, obj_stream, &user_input_obj, &obj_err)) {
            auto content_array = user_input_obj["data"]["content"];
            if (content_array.isArray()) {
                for (const auto& item : content_array) {
                    std::string item_type = item.get("type", "").asString();
                    if (item_type == "image") {
                        Json::Value image_part;
                        image_part["type"] = "image_url";
                        image_part["image_url"]["url"] = item.get("data", "").asString();
                        content_parts.append(image_part);
                    }
                }
            }
        }
    }

    // content 直接传 ContentPart 数组（JSON 数组，非字符串化）
    // bridge 的 normalize_content 需要数组才能识别 image_url part 并落盘；
    // 纯文本场景传 [{type:text,text:...}] 数组，bridge prepare_content_for_daemon
    // 同样返回纯文本，行为一致（对齐 main.cpp forward_to_bridge）。

    // 提取上游回调地址（SysAgentEntity）
    std::string sys_agent_entity = header.get("SysAgentEntity", "").asString();

    // 5. 服务发现
    Json::Value services(Json::objectValue);
    for (const auto& name : split_services(std::getenv("COREMIND_REQUIRED_SERVICES"))) {
        auto snap = service_snapshot(name);
        if (snap["status"].asString() != "resolved") {
            error_response(context, 503,
                "REQUIRED_SERVICE_DISCOVERY_FAILED: " + name);
            return CMFrm::COM::SYNC;
        }
        services[name] = snap;
    }
    for (const auto& name : split_services(std::getenv("COREMIND_OPTIONAL_SERVICES"))) {
        services[name] = service_snapshot(name);
    }

    // 6. 生成 request_id
    static std::atomic<unsigned long long> counter{0};
    std::string request_id = "sidecar-" + std::to_string(++counter);
    if (seq_num.empty()) {
        seq_num = request_id;  // 上游未带 SeqNum 时回退 request_id，保证回调 Header.SeqNum 非空
    }

    std::string bridge_url = bridge_url_;  // 由 main.cpp --bridge-url 注入（set_bridge_url）
    // 向后兼容：环境变量 BRIDGE_URL 仍可覆盖（如未通过 --bridge-url 传入时）
    if (const char* env_url = std::getenv("BRIDGE_URL")) {
        bridge_url = env_url;
    }
    // 转发目标 agent：由 main.cpp 从 configs/{env}/sidecar.json 注入（env 覆盖后），
    // 替换旧硬编码 outfit-assistant。默认值仍为 outfit-assistant 保持向后兼容。
    std::string agent_id = agent_name_;
    // 从 bridge_url 提取 path prefix（对齐 main.cpp parse_bridge_target）：
    // bridge 业务路由挂载在 /health 前缀下（main.py app.mount("/health", bridge_app)），
    // 转发路径必须拼上该前缀，否则 404。--bridge-url 应含 /health（如 http://127.0.0.1:8199/health）。
    std::string bridge_path_prefix;
    {
        auto scheme_pos = bridge_url.find("://");
        auto auth_start = (scheme_pos == std::string::npos) ? 0 : scheme_pos + 3;
        auto path_start = bridge_url.find('/', auth_start);
        if (path_start != std::string::npos) {
            bridge_path_prefix = bridge_url.substr(path_start);
            while (bridge_path_prefix.size() > 1 && bridge_path_prefix.back() == '/') {
                bridge_path_prefix.pop_back();
            }
            if (bridge_path_prefix == "/") bridge_path_prefix.clear();
        }
    }
    std::string callback_path = bridge_path_prefix + "/api/chat/" + agent_id + "?stream=false";

    // ── 有 SysAgentEntity → 异步模式；无 → 同步 dev 模式 ──
    if (!sys_agent_entity.empty()) {
        // 只返回 200 status code（对齐 SysAgent），实际结果通过回调 POST 送回上游
        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);

        // 后台线程处理（捕获所有需要的数据）
        std::thread([=]() {
            auto scheme_pos = bridge_url.find("://");
            auto auth_start = (scheme_pos == std::string::npos) ? 0 : scheme_pos + 3;
            auto path_start = bridge_url.find('/', auth_start);
            std::string origin = (path_start == std::string::npos)
                                     ? bridge_url
                                     : bridge_url.substr(0, path_start);

            // 构造 REST API 请求体
            Json::Value rest_body;
            rest_body["user_id"] = user_name;
            rest_body["client_id"] = client_session_id;
            rest_body["content"] = content_parts;

            // 回调 POST 辅助函数（dumper 必须先于本 lambda 声明——lambda [&] 捕获不到尚未声明的变量）
            auto dumper = std::make_shared<sidecar::SidecarDumper>(request_id);
            auto post_callback = [&](const std::string& stream_status, int flag,
                                       const std::string& text, bool is_final,
                                       const std::string& user_output_object) -> bool {
                dumper->dump_callback(stream_status, flag, text, is_final, user_output_object);
                Json::Value cb;
                cb["Data"]["MessageType"] = "UserOutput";
                cb["Data"]["StreamStatus"] = stream_status;
                cb["Data"]["UserOutputFlag"] = flag;
                cb["Data"]["UserOutputText"] = text;
                cb["Data"]["UserOutputObject"] = user_output_object;
                cb["Data"]["UserManualInterventionFlag"] = false;
                cb["Data"]["ImplicitOutputFlag"] = false;
                cb["Data"]["IsVoiceBroadcast"] = 1;
                cb["Header"] = build_callback_header("DataExchange", task_id, dialogue_id,
                                                      user_name, client_session_id,
                                                      sender, receiver, seq_num, req_header);
                cb["Header"]["EventTimeStamp"] = current_timestamp_iso8601();
                std::string target = "https://" + sys_agent_entity + ":23767"
                                   + "/coremind/systemagent/v1/callback";
                std::string cb_str = json_dumps(cb);
                dumper->dump_callback_request("user_output", target, cb_str);
                // F7.1 原样打印发送给上游的完整请求体(用户要求,不截断)
                std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_REQUEST req=" << request_id
                          << " status=" << stream_status << " flag=" << flag
                          << " target=" << target
                          << " body=" << cb_str << std::endl;
#ifdef COREMIND_ENABLED
                auto httpF = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
                auto h2 = httpF->GetHttp2Client("default");
                if (h2) {
                    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
                    req->SetMethod(CMFrm::COM::POST);
                    req->SetReqUrl(target);
                    req->AddHeader("Content-Type", "application/json");
                    req->AddBody(cb_str);
                    auto h2_res = h2->Send(req);
                    if (h2_res && h2_res->Is2XXStatusCode()) {
                        // F7.1 原样打印上游响应体(成功也完整打印,用户要求不截断)
                        std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_RESP req=" << request_id
                                  << " via=coremind_h2 http=" << h2_res->GetStatusCode()
                                  << " resp_body=" << h2_res->GetResponseBody() << std::endl;
                        dumper->dump_callback_result("user_output", "coremind_h2", h2_res->GetStatusCode(),
                                                      "", h2_res->GetResponseBody());
                        return true;
                    }
                    // H2 发送失败/非 2xx:明确报错,不回退 httplib 重发(用户要求,避免重复回调)
                    std::string h2_detail = !h2_res ? "null response" : h2_res->GetResponseBody();
                    std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_ERR req=" << request_id
                              << " via=coremind_h2 http=" << (h2_res ? h2_res->GetStatusCode() : 0)
                              << " error=H2_SEND_FAILED resp_body=" << h2_detail << std::endl;
                    dumper->dump_callback_result("user_output", "coremind_h2",
                                                  h2_res ? h2_res->GetStatusCode() : 0,
                                                  h2_res ? "H2_SEND_FAILED" : "H2_SEND_NULL_RESPONSE",
                                                  h2_res ? h2_res->GetResponseBody() : "");
                    return false;
                }
                std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_ERR req=" << request_id
                          << " via=coremind_h2 error=H2_CLIENT_UNAVAILABLE target=" << target << std::endl;
                dumper->dump_callback_result("user_output", "coremind_h2", 0, "H2_CLIENT_UNAVAILABLE", "");
                return false;
#else
                // 本文件仅生产模式(COREMIND_ENABLED=ON)编译;此处兜底避免未定义行为
                std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_ERR req=" << request_id
                          << " error=PROD_ONLY_PATH" << std::endl;
                dumper->dump_callback_result("user_output", "stub", 0, "PROD_ONLY_PATH", "");
                return false;
#endif
            };

            std::string callback_path_stream = bridge_path_prefix + "/api/chat/" + agent_id + "?stream=true";
            httplib::Client cli(origin.c_str());
            cli.set_read_timeout(360, 0);
            httplib::Headers http_headers;
            http_headers.emplace("Content-Type", "application/json");
            http_headers.emplace("X-Request-Id", request_id);

            std::string sse_buffer;
            sidecar::SSEChunkProcessor processor;
            std::string body_str = json_dumps(rest_body);
            dumper->dump_request(rest_body);
            std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] FWD " << callback_path_stream << " req=" << request_id
                      << " | body=\n" << pretty_json(body_str) << std::endl;
            // P3: Post 只用 ContentReceiver（去掉 Progress callback）。
            // 任务结束逻辑统一放到 Post 返回后，确保任何退出路径（成功/失败/超时）
            // 都向 upstream 发送 TaskStatusReport，防止 upstream 永久等待。
            // [DONE] 不再 return false（原实现导致 httplib 中止接收、Progress 不触发，
            // 任务结束消息永远发不出去），改为 continue 让 Post 自然完成。
            auto stream_result = cli.Post(callback_path_stream.c_str(), http_headers,
                                          body_str, "application/json",
                [&](const char* data, size_t len) -> bool {
                    sse_buffer.append(data, len);
                    // 按 \n 分割 SSE 事件
                    size_t pos;
                    while ((pos = sse_buffer.find('\n')) != std::string::npos) {
                        std::string line = sse_buffer.substr(0, pos);
                        sse_buffer.erase(0, pos + 1);
                        if (line.empty()) continue;  // 空行（SSE 事件间分隔）
                        // 去掉行尾回车
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        // 只处理 data: 行
                        if (line.compare(0, 6, "data: ") != 0) continue;
                        std::string payload = line.substr(6);
                        // [DONE] 标记流结束，不阻断接收（让 Post 自然返回后统一处理）
                        if (payload == "[DONE]") continue;
                        std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] SSE_RAW len=" << payload.size()
                                  // F7 展示层:payload 可能含 _artifacts data_base64,字符串级截断
                                  << " data=" << sidecar::truncate_base64_in_text(payload) << std::endl;
                        // 解析 SSE json
                        Json::Value sse_json;
                        Json::CharReaderBuilder rdr;
                        std::string err;
                        std::istringstream ss(payload);
                        if (!Json::parseFromStream(rdr, ss, &sse_json, &err)) continue;
                        dumper->dump_sse_chunk(sse_json);
                        for (const auto& cb : processor.process(sse_json)) {
                            post_callback(cb.stream_status, cb.flag, cb.text,
                                          cb.is_final, cb.user_output_object);
                        }
                    }
                    return true;
                }
            );

            // ── 任务结束：无论 bridge 成功/失败/超时都必须通知 upstream ──
            // 错误分流（对齐 dev main.cpp forward_to_bridge + bridge chat_rest.py 错误两路）：
            //   ① HTTP 不可达/断开/超时 → ts=500/BridgeError
            //   ② HTTP 非 200          → ts=resp.status/BridgeError，body 含 error detail
            //   ③ HTTP 200 但 SSE 流内 error payload → ts=500/BridgeError，error_message_ 携详情
            //   ④ HTTP 200 且无错误    → ts=200/TaskCompleted，先发 Completed UserOutput 再发 TaskStatus
            // 错误时跳过 Completed 回调（避免上游收到虚假"完成"信号），仅发错误 TaskStatus。
            int ts_code;
            std::string ts_status;
            std::string ts_error_detail;  // 提取的 bridge 错误详情，拼到 TaskStatusReport 让上游可定位
            if (!stream_result) {
                ts_code = 500;
                ts_status = "BridgeError";
                ts_error_detail = httplib::to_string(stream_result.error());
                std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] BRIDGE_ERR req=" << request_id
                          << " error=" << ts_error_detail << std::endl;
            } else if (stream_result->status != 200) {
                ts_code = stream_result->status;
                ts_status = "BridgeError";
                ts_error_detail = stream_result->body.substr(0, 500);
                std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] BRIDGE_HTTP_ERR req=" << request_id
                          << " http=" << stream_result->status
                          << " body=" << ts_error_detail << std::endl;
            } else if (processor.has_error()) {
                // bridge SSE 流内 error payload（chat_rest.py:1004-1021 daemon 通知中途失败/timeout）
                ts_code = 500;
                ts_status = "BridgeError";
                ts_error_detail = processor.error_message();
                std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] SSE_STREAM_ERR req=" << request_id
                          << " error=" << ts_error_detail << std::endl;
            } else {
                ts_code = 200;
                ts_status = "TaskCompleted";
                std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] STREAM_DONE req=" << request_id
                          << " content_len=" << processor.accumulated_content().size()
                          << std::endl;
            }

            // 仅 bridge 完全成功时发 Completed UserOutput 回调；错误时上游只收 TaskStatus（避免虚假完成）
            if (ts_code == 200) {
                auto completed = processor.make_completed();
                // 产物 ACM 上传（L4 §4.2）：url 优先下载 → base64 兜底 → upload_file → 写 acm_file_id。
                // 失败降级保留原 url，不阻断回调（I-degrade / I-acm-file-id）。
                if (!completed.user_output_object.empty()) {
                    auto uoo = parse_request_body(completed.user_output_object);
                    if (uoo) {
                        const int injected = sidecar::enrich_artifacts_with_acm(
                            *uoo,
                            [](const std::string& url) { return sidecar::AcmClient::fetch_url(url); },
                            [](const std::string& name, const std::string& content,
                               const std::string& mime) {
                                const std::vector<uint8_t> bytes(content.begin(), content.end());
                                const auto res = sidecar::AcmClient::upload_file(name, bytes, mime);
                                return res.success ? res.file_uuid : "";
                            });
                        if (injected > 0) {
                            completed.user_output_object = json_dumps(*uoo);
                            std::cerr << "[" << sidecar::log_timestamp()
                                      << "] [sidecar_router] ACM_ART_INJECT req=" << request_id
                                      << " injected=" << injected << std::endl;
                        }
                    }
                }
                post_callback(completed.stream_status, completed.flag, completed.text,
                              completed.is_final, completed.user_output_object);
            }

            // TaskStatusReport（成功/失败都发，防止 upstream 永久等待）
            Json::Value task_status;
            // TaskStatusManagement 是任务结束信号：Header.Receiver 固定为 SystemAgent
            // （AISF 协议 §2.2，与 UserOutput 的 Receiver=请求 Sender 不同），
            // SeqNum 透传上游请求 Header.SeqNum（router_logic.h build_callback_header）。
            task_status["Header"] = build_callback_header(
                "TaskStatusManagement", task_id, dialogue_id,
                user_name, client_session_id,
                sender, receiver, seq_num, req_header, "SystemAgent");
            task_status["Header"]["EventTimeStamp"] = current_timestamp_iso8601();
            Json::Value ts_data;
            ts_data["MessageType"] = "TaskStatusReport";
            ts_data["TaskStatusCode"] = ts_code;
            ts_data["TaskStatusReport"] = ts_status;
            // 错误时把 bridge 详情拼到 UserOutputText，上游可据此定位（F7 错误可定位 + F1 禁止掩盖）
            // 成功时仍是 coordinator 的最终回答 accumulated_content。
            ts_data["UserOutputText"] = ts_code == 200
                ? processor.accumulated_content()
                : ("bridge_error: " + ts_error_detail);
            task_status["Data"] = ts_data;
            std::string ts_url = "https://" + sys_agent_entity + ":23767"
                               + "/coremind/systemagent/v1/callback";
            std::string ts_str = json_dumps(task_status);
            dumper->dump_callback_request("task_status", ts_url, ts_str);
            // F7.1 原样打印发送给上游的完整请求体(用户要求,不截断)
            std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_REQUEST req=" << request_id
                      << " type=task_status code=" << ts_code << " status=" << ts_status
                      << " target=" << ts_url
                      << " body=" << ts_str << std::endl;

            // 发送 TaskStatusReport（CoreMind H2 唯一通道;失败明确报错,不回退 httplib 重发）
#ifdef COREMIND_ENABLED
            {
                auto httpF = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
                auto h2 = httpF->GetHttp2Client("default");
                if (h2) {
                    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
                    req->SetMethod(CMFrm::COM::POST);
                    req->SetReqUrl(ts_url);
                    req->AddHeader("Content-Type", "application/json");
                    req->AddBody(ts_str);
                    auto h2_res = h2->Send(req);
                    if (h2_res && h2_res->Is2XXStatusCode()) {
                        // F7.1 原样打印上游响应体(成功也完整打印,用户要求不截断)
                        std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_RESP req=" << request_id
                                  << " via=coremind_h2 type=task_status http=" << h2_res->GetStatusCode()
                                  << " resp_body=" << h2_res->GetResponseBody() << std::endl;
                        dumper->dump_callback_result("task_status", "coremind_h2", h2_res->GetStatusCode(),
                                                      "", h2_res->GetResponseBody());
                    } else {
                        // H2 发送失败/非 2xx:明确报错,不回退 httplib 重发(用户要求,避免重复回调)
                        std::string h2_detail = !h2_res ? "null response" : h2_res->GetResponseBody();
                        std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_ERR req=" << request_id
                                  << " via=coremind_h2 type=task_status http=" << (h2_res ? h2_res->GetStatusCode() : 0)
                                  << " error=H2_SEND_FAILED resp_body=" << h2_detail << std::endl;
                        dumper->dump_callback_result("task_status", "coremind_h2",
                                                      h2_res ? h2_res->GetStatusCode() : 0,
                                                      h2_res ? "H2_SEND_FAILED" : "H2_SEND_NULL_RESPONSE",
                                                      h2_res ? h2_res->GetResponseBody() : "");
                    }
                } else {
                    std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_ERR req=" << request_id
                              << " via=coremind_h2 type=task_status error=H2_CLIENT_UNAVAILABLE target=" << ts_url << std::endl;
                    dumper->dump_callback_result("task_status", "coremind_h2", 0, "H2_CLIENT_UNAVAILABLE", "");
                }
            }
#else
            std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] CBF_ERR req=" << request_id
                      << " error=PROD_ONLY_PATH" << std::endl;
            dumper->dump_callback_result("task_status", "stub", 0, "PROD_ONLY_PATH", "");
#endif
        }).detach();

        return CMFrm::COM::ASYNC;
    }

    // ===== 同步模式（httplib dev，无上游回调地址） =====
    auto scheme_pos = bridge_url.find("://");
    auto auth_start = (scheme_pos == std::string::npos) ? 0 : scheme_pos + 3;
    auto path_start = bridge_url.find('/', auth_start);
    std::string origin = (path_start == std::string::npos)
                             ? bridge_url
                             : bridge_url.substr(0, path_start);

    // 构造 REST API 请求体（ChatRestRequest 格式）
    Json::Value rest_body;
    rest_body["user_id"] = user_name;
    rest_body["client_id"] = client_session_id;
    rest_body["content"] = content_parts;

    httplib::Client cli(origin.c_str());
    cli.set_read_timeout(360, 0);
    httplib::Headers http_headers;
    http_headers.emplace("Content-Type", "application/json");
    http_headers.emplace("X-Request-Id", request_id);

    std::string body_str = json_dumps(rest_body);
    std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_router] FWD " << callback_path << " req=" << request_id
              << " | body=\n" << pretty_json(body_str) << std::endl;
    auto response = cli.Post(callback_path.c_str(), http_headers, body_str, "application/json");
    if (!response) {
        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP500);
        return CMFrm::COM::SYNC;
    }

    std::string resp_text;
    if (!response->body.empty()) {
        Json::Value bridge_resp;
        Json::CharReaderBuilder resp_reader;
        std::string resp_errors;
        std::istringstream resp_input(response->body);
        if (Json::parseFromStream(resp_reader, resp_input, &bridge_resp, &resp_errors)) {
            resp_text = bridge_resp.get("content", "").asString();
        } else {
            resp_text = response->body;
        }
    }

    std::string resp_preview = resp_text.substr(0, 500);
    if (resp_text.size() > 500) resp_preview += "... (truncated)";
    std::cout << "[" << sidecar::log_timestamp() << "] [sidecar_router] sync response: request_id=" << request_id
              << " task_id=" << task_id << " resp_len=" << resp_text.size()
              // F7.1: 完整响应体原样打印(用户要求不截断),preview 仅供快速浏览
              << " resp_body=" << response->body << std::endl;

    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode SidecarRouter::HandleAgentInstall(
    const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    ok_response(context, Json::Value(Json::objectValue));
    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode SidecarRouter::HandleAgentUpdate(
    const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    ok_response(context, Json::Value(Json::objectValue));
    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode SidecarRouter::HandleAgentUninstall(
    const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    ok_response(context, Json::Value(Json::objectValue));
    return CMFrm::COM::SYNC;
}
