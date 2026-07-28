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
// 对标 HealthAgent 的模式：WriteStatueCode + WriteJSONContentType + WriteResponseBody
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
    Json::Value body;
    body["status"] = "healthy";
    body["coremind"] = true;
    body["sidecar"] = true;
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
    // 1. 读取请求体
    std::string req_body = context->GetRequestBody();
    if (req_body.empty()) {
        error_response(context, 400, "empty request body");
        return CMFrm::COM::SYNC;
    }

    // 2. 解析请求体为 JSON
    Json::Value original_request;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(req_body);
    if (!Json::parseFromStream(reader, input, &original_request, &errors)) {
        error_response(context, 400, "invalid JSON: " + errors);
        return CMFrm::COM::SYNC;
    }

    // 3. 从 AccessCtrl 格式提取字段（全部保存用于响应回传）
    auto header = original_request["Header"];
    auto data = original_request["Data"];
    Json::Value req_header = header;  // 保存完整 Header 用于接口转换
    std::string user_name = header.get("UserName", "").asString();
    std::string client_session_id = header.get("ClientSessionId",
                               header.get("SessionId", "")).asString();
    std::string task_id = header.get("TaskID", "").asString();
    std::string dialogue_id = header.get("DialogueId", "").asString();
    std::string sender = header.get("Sender", "").asString();
    std::string receiver = header.get("Receiver", "").asString();
    std::string user_input_text = data.get("UserInputText", "").asString();

    // 4. 构建多模态 content（OpenAI 格式）
    // 合并 UserInputText + UserInputObject.data.content[]
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

    Json::Value messages(Json::arrayValue);
    Json::Value user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = content_parts;
    messages.append(user_msg);
    std::string content_str = json_dumps(messages);

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

    // ── 异步回调模式 ──
    // SideCar 模式：立即返回 accepted → 后台处理 → POST 结果到上游回调 URL
    // httplib 模式（dev）：直接同步返回（无上游回调地址）
    if (!sys_agent_entity.empty()) {
        // ===== 异步模式：立即返回 accepted，后台处理 + POST 回上游 =====
        // 立即返回 accepted
        Json::Value accepted;
        accepted["status"] = "accepted";
        accepted["task_id"] = task_id;
        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
        context->WriteJSONContentType();
        context->WriteResponseBody(json_dumps(accepted));

        // 后台线程处理（捕获所有需要的数据）
        std::thread([=, ctx = context]() {
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
                                     const std::string& text, bool is_final) {
                Json::Value cb;
                cb["Data"]["MessageType"] = "UserOutput";
                cb["Data"]["StreamStatus"] = stream_status;
                cb["Data"]["UserOutputFlag"] = flag;
                cb["Data"]["UserOutputText"] = text;
                cb["Header"]["MessageType"] = "DataExchange";
                cb["Header"]["TaskID"] = task_id;
                cb["Header"]["DialogueId"] = dialogue_id;
                cb["Header"]["UserName"] = user_name;
                cb["Header"]["ClientSessionId"] = client_session_id;
                cb["Header"]["Sender"] = receiver.empty() ? "SysAgent" : receiver;
                cb["Header"]["Receiver"] = sender.empty() ? "APP" : sender;
                cb["Header"]["SeqNum"] = request_id;
                cb["Header"]["EventTimeStamp"] = current_timestamp_ms();
                for (const auto& k : {"AcmSessionId", "ClientExField", "DeviceCategory",
                                      "DeviceId", "SessionId", "TeamEnvironmentID", "UUID"}) {
                    if (req_header.isMember(k)) cb["Header"][k] = req_header[k];
                }
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
                cb_cli.Post(target.substr(dp).c_str(),
                           {{"Content-Type", "application/json"}},
                           cb_str, "application/json");
            };

            std::string callback_path_stream = "/api/chat/" + agent_id + "?stream=true";
            httplib::Client cli(origin.c_str());
            cli.set_read_timeout(360, 0);
            httplib::Headers http_headers;
            http_headers.emplace("Content-Type", "application/json");
            http_headers.emplace("X-Request-Id", request_id);

            std::string sse_buffer;
            std::string accumulated_content;
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
                        // ── 只返回 coordinator 的消息 ──
                        // skill_executor 的消息（_source=skill_executor）跳过，不回调上游。
                        // 如需恢复所有消息，注释掉下面三行，取消注释 next 三行。
                        std::string _src = sse_json.get("_source", "").asString();
                        if (_src != "coordinator" && !_src.empty()) continue;
                        // ── end ──
                        bool is_progress = sse_json.get("_progress", false).asBool()
                                        || sse_json.get("_chunk_type", "").asString() == "progress";
                        // 原来的代码（取消注释可恢复全量回调）:
                        // bool is_progress = sse_json.get("_progress", false).asBool()
                        //                 || sse_json.get("_chunk_type", "").asString() == "progress";
                        // post_callback("Running", is_progress ? 1 : 0, token, false);
                        post_callback("Running", is_progress ? 1 : 0, token, false);
                        accumulated_content += token;
                    }
                    return true;
                },
                [&](uint64_t, uint64_t) -> bool {
                    // SSE 流结束：发送 Completed 信号（内容已增量发出，Completed 仅做结束标记）
                    post_callback("Completed", 0, "", true);

                    // 额外发送 TaskStatus 报文
                    Json::Value task_status;
                    Json::Value hdr;
                    hdr["MessageType"] = "TaskStatusManagement";
                    hdr["TaskID"] = task_id;
                    hdr["DialogueId"] = dialogue_id;
                    hdr["Sender"] = receiver.empty() ? "SysAgent" : receiver;
                    hdr["Receiver"] = sender.empty() ? "APP" : sender;
                    hdr["UserName"] = user_name;
                    hdr["ClientSessionId"] = client_session_id;
                    hdr["SeqNum"] = request_id;
                    hdr["EventTimeStamp"] = current_timestamp_ms();
                    task_status["Header"] = hdr;
                    Json::Value data;
                    data["MessageType"] = "TaskStatusReport";
                    data["TaskStatusCode"] = 200;
                    data["TaskStatusReport"] = "TaskCompleted";
                    data["UserOutputText"] = accumulated_content;
                    task_status["Data"] = data;
                    // 添加透传字段到 TaskStatus Header
                    for (const auto& k : {"AcmSessionId", "ClientExField", "DeviceCategory",
                                          "DeviceId", "SessionId", "TeamEnvironmentID", "UUID"}) {
                        if (req_header.isMember(k))
                            task_status["Header"][k] = req_header[k];
                    }
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

                    ctx->WriteAsyncResponse();
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
        error_response(context, 502, "BRIDGE_UNREACHABLE");
        return CMFrm::COM::SYNC;
    }

    // 解析 REST API 响应（ChatRestResponse 格式：{task_id, content, artifacts}）
    Json::Value bridge_resp;
    Json::CharReaderBuilder resp_reader;
    std::string resp_errors;
    std::istringstream resp_input(response->body);
    std::string resp_text;
    std::string task_id_resp = task_id;
    Json::Value resp_artifacts;
    if (Json::parseFromStream(resp_reader, resp_input, &bridge_resp, &resp_errors)) {
        resp_text = bridge_resp.get("content", "").asString();
        task_id_resp = bridge_resp.get("task_id", task_id).asString();
        resp_artifacts = bridge_resp["artifacts"];
    } else {
        resp_text = response->body;
    }

    // ── UserOutput 报文（流式完成） ──
    {
        Json::Value ack;
        Json::Value ack_data;
        ack_data["MessageType"] = "UserOutput";
        ack_data["StreamStatus"] = "Completed";
        ack_data["UserOutputFlag"] = 0;
        ack_data["ImplicitOutputFlag"] = false;
        ack_data["IsVoiceBroadcast"] = 1;
        ack_data["UserManualInterventionFlag"] = false;
        ack_data["UserOutputText"] = resp_text;
        if (resp_artifacts.isArray() && resp_artifacts.size() > 0) {
            ack_data["UserOutputObject"]["code"] = 200101;
            ack_data["UserOutputObject"]["data"]["content"] = resp_artifacts;
        } else {
            ack_data["UserOutputObject"] = "";
        }
        ack["Data"] = ack_data;

        Json::Value ack_header;
        ack_header["MessageType"] = "DataExchange";
        ack_header["TaskID"] = task_id_resp;
        ack_header["DialogueId"] = dialogue_id;
        ack_header["UserName"] = user_name;
        ack_header["ClientSessionId"] = client_session_id;
        ack_header["Sender"] = receiver.empty() ? "SysAgent" : receiver;
        ack_header["Receiver"] = sender.empty() ? "APP" : sender;
        ack_header["SeqNum"] = request_id;
        ack_header["EventTimeStamp"] = current_timestamp_ms();
        // 回传上游原 Header 中的透传字段
        for (const auto& key : {"AcmSessionId", "ClientExField", "DeviceCategory", "DeviceId",
                                "SessionId", "SysAgentEntity", "TeamEnvironmentID", "UUID"}) {
            if (req_header.isMember(key)) {
                ack_header[key] = req_header[key];
            }
        }
        ack["Header"] = ack_header;

        // 同步模式下，UserOutput 报文暂不写回，保留供流式实现后使用
        std::cout << "[sidecar_router] user_output: " << json_dumps(ack) << std::endl;
    }

    // ── TaskStatus 报文（任务完成通知，同步模式写回上游） ──
    {
        Json::Value ts;
        Json::Value ts_header;
        ts_header["MessageType"] = "TaskStatusManagement";
        ts_header["Sender"] = receiver.empty() ? "SysAgent" : receiver;
        ts_header["Receiver"] = sender.empty() ? "APP" : sender;
        ts_header["UserName"] = user_name;
        ts_header["ClientSessionId"] = client_session_id;
        ts_header["TaskID"] = task_id_resp;
        ts_header["DialogueId"] = dialogue_id;
        ts_header["SeqNum"] = request_id;
        ts_header["EventTimeStamp"] = current_timestamp_ms();
        for (const auto& key : {"AcmSessionId", "ClientExField", "DeviceCategory", "DeviceId",
                                "SessionId", "SysAgentEntity", "TeamEnvironmentID", "UUID"}) {
            if (req_header.isMember(key)) {
                ts_header[key] = req_header[key];
            }
        }
        ts["Header"] = ts_header;

        Json::Value ts_data;
        ts_data["MessageType"] = "TaskStatusReport";
        ts_data["TaskStatusCode"] = resp_text.empty() ? 500 : 200;
        ts_data["TaskStatusReport"] = resp_text.empty() ? "TaskFailed" : "TaskCompleted";
        ts_data["UserOutputText"] = resp_text;
        ts["Data"] = ts_data;

        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
        context->WriteJSONContentType();
        context->WriteResponseBody(json_dumps(ts));
    }

    std::cout << "[sidecar_router] forwarded to bridge: request_id="
              << request_id << " task_id=" << task_id_resp << std::endl;
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
