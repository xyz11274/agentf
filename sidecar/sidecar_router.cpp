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

using sidecar::build_callback_header;
using sidecar::build_health_response;
using sidecar::build_user_output_object;
using sidecar::parse_request_body;

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
    body["agent_name"] = get_env_or("AGENT_ID", "outfit-assistant");
    body["version"] = "1.0.0";
    body["capabilities"] = Json::Value(Json::arrayValue);
    body["capabilities"].append("outfit_consult");
    body["capabilities"].append("weather_query");
    body["capabilities"].append("occasion_guide");
    body["capabilities"].append("image_generation");
    body["capabilities"].append("video_generation");
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

    Json::Value req_header = header;
    std::string user_name = header.get("UserName", "").asString();
    std::string client_session_id = header.get("ClientSessionId",
                               header.get("SessionId", "")).asString();
    std::string task_id = header.get("TaskID", "").asString();
    std::string dialogue_id = header.get("DialogueId", "").asString();
    std::string sender = header.get("Sender", "").asString();
    std::string receiver = header.get("Receiver", "").asString();
    std::string user_input_text = data.get("UserInputText", "").asString();

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

    // content 直接传 ContentPart 数组，不包装 messages 结构
    // Bridge 的 _chat_turn_events 会自行包装 role/user（对齐 main.cpp forward_to_bridge）
    std::string content_str;
    if (content_parts.size() == 1 && content_parts[0].isMember("text")) {
        // 纯文本场景：直接传字符串
        content_str = content_parts[0]["text"].asString();
    } else {
        // 多模态场景：传 ContentPart JSON 数组
        content_str = json_dumps(content_parts);
    }

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

    std::string bridge_url = get_env_or("BRIDGE_URL", "http://127.0.0.1:8199");
    std::string agent_id = get_env_or("AF_BR_COREMIND_AGENT_ID", "outfit-assistant");
    std::string callback_path = "/api/chat/" + agent_id + "?stream=false";

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
            rest_body["content"] = content_str;

            // 回调 POST 辅助函数
            auto post_callback = [&](const std::string& stream_status, int flag,
                                      const std::string& text, bool is_final,
                                      const std::string& user_output_object) -> bool {
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
                                                      sender, receiver, request_id, req_header);
                cb["Header"]["EventTimeStamp"] = current_timestamp_ms();
                std::string target = "https://" + sys_agent_entity + ":23767"
                                   + "/coremind/systemagent/v1/callback";
                std::string cb_str = json_dumps(cb);
#ifdef COREMIND_ENABLED
                auto httpF = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
                auto h2 = httpF->GetHttp2Client("default");
                if (h2) {
                    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
                    req->SetMethod(CMFrm::COM::POST);
                    req->SetReqUrl(target);
                    req->AddHeader("Content-Type", "application/json");
                    req->AddBody(cb_str);
                    h2->Send(req);
                    return true;
                }
#endif
                auto ds = target.find("://");
                auto da = ds == std::string::npos ? 0 : ds + 3;
                auto dp = target.find('/', da);
                httplib::Client cb_cli(target.substr(0, dp).c_str());
                auto cb_res = cb_cli.Post(target.substr(dp).c_str(),
                                       {{"Content-Type", "application/json"}},
                                       cb_str, "application/json");
                // httplib::Result 提供 operator bool()：请求成功（拿到 Response）为 true，
                // 连接失败/未发出为 false。lambda 返回值未被任何调用点消费，
                // 此处仅用于消除 lambda 内两分支返回类型不一致的编译错误。
                return static_cast<bool>(cb_res);
            };

            std::string callback_path_stream = "/api/chat/" + agent_id + "?stream=true";
            httplib::Client cli(origin.c_str());
            cli.set_read_timeout(360, 0);
            httplib::Headers http_headers;
            http_headers.emplace("Content-Type", "application/json");
            http_headers.emplace("X-Request-Id", request_id);

            std::string sse_buffer;
            std::string accumulated_content;
            Json::Value accumulated_artifacts;
            cli.Post(callback_path_stream.c_str(), http_headers, json_dumps(rest_body), "application/json",
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
                        // 检查终止信号
                        if (payload == "[DONE]") return false;
                        // 解析 SSE json
                        Json::Value sse_json;
                        Json::CharReaderBuilder rdr;
                        std::string err;
                        std::istringstream ss(payload);
                        if (!Json::parseFromStream(rdr, ss, &sse_json, &err)) continue;
                        Json::Value chunk_artifacts = sse_json.get("_artifacts", Json::Value(Json::nullValue));
                        if (!chunk_artifacts.isNull()) {
                            accumulated_artifacts = chunk_artifacts;
                        }
                        // 提取 content
                        std::string token;
                        auto choices = sse_json["choices"];
                        if (choices.isArray() && choices.size() > 0) {
                            auto delta = choices[0]["delta"]["content"];
                            if (delta.isString()) token = delta.asString();
                            else {
                                auto msg = choices[0]["message"]["content"];
                                if (msg.isString()) token = msg.asString();
                            }
                        }
                        if (token.empty()) continue;
                        std::string _src = sse_json.get("_source", "").asString();
                        if (_src != "coordinator" && !_src.empty()) continue;
                        bool is_progress = sse_json.get("_progress", false).asBool()
                                        || sse_json.get("_chunk_type", "").asString() == "progress";
                        post_callback("Running", is_progress ? 1 : 0, token, false, "");
                        accumulated_content += token;
                    }
                    return true;
                },
                [&](uint64_t, uint64_t) -> bool {
                    // SSE 流结束：发送 Completed 信号（内容已增量发出，Completed 仅做结束标记）
                    std::string output_obj = build_user_output_object(accumulated_artifacts);
                    post_callback("Completed", 0, "", true, output_obj);

                    // 额外发送 TaskStatus 报文
                    Json::Value task_status;
                    task_status["Header"] = build_callback_header(
                        "TaskStatusManagement", task_id, dialogue_id,
                        user_name, client_session_id,
                        sender, receiver, request_id, req_header);
                    task_status["Header"]["EventTimeStamp"] = current_timestamp_ms();
                    Json::Value data;
                    data["MessageType"] = "TaskStatusReport";
                    data["TaskStatusCode"] = 200;
                    data["TaskStatusReport"] = "TaskCompleted";
                    data["UserOutputText"] = accumulated_content;
                    task_status["Data"] = data;
                    std::string ts_url = "https://" + sys_agent_entity + ":23767"
                                       + "/coremind/systemagent/v1/callback";
                    std::string ts_str = json_dumps(task_status);
#ifdef COREMIND_ENABLED
                    auto httpF = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
                    auto h2 = httpF->GetHttp2Client("default");
                    if (h2) {
                        auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
                        req->SetMethod(CMFrm::COM::POST);
                        req->SetReqUrl(ts_url);
                        req->AddHeader("Content-Type", "application/json");
                        req->AddBody(ts_str);
                        h2->Send(req);
                        return true;
                    }
#endif
                    auto ds = ts_url.find("://");
                    auto da = ds == std::string::npos ? 0 : ds + 3;
                    auto dp = ts_url.find('/', da);
                    httplib::Client cb_cli(ts_url.substr(0, dp).c_str());
                    cb_cli.Post(ts_url.substr(dp).c_str(),
                               {{"Content-Type", "application/json"}},
                               ts_str, "application/json");

                    return true;
                }
            );
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
    rest_body["content"] = content_str;

    httplib::Client cli(origin.c_str());
    cli.set_read_timeout(360, 0);
    httplib::Headers http_headers;
    http_headers.emplace("Content-Type", "application/json");
    http_headers.emplace("X-Request-Id", request_id);

    auto response = cli.Post(callback_path.c_str(), http_headers, json_dumps(rest_body), "application/json");
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

    std::cout << "[sidecar_router] sync response: request_id=" << request_id
              << " task_id=" << task_id << " resp_len=" << resp_text.size() << std::endl;

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
