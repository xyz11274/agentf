#include "sidecar_router.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <atomic>
#include <vector>

#include <json/json.h>
#include <httplib.h>

#include "resolve_handler.h"

// ── JSON 辅助 ──

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

    // 3. 服务发现（对标 HealthAgent 的 resolve + forward 模式）
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

    // 4. 构造 trusted envelope → 转发到 Bridge（HTTP/1.1 即可，Bridge 是 FastAPI）
    static std::atomic<unsigned long long> counter{0};
    std::string request_id = "sidecar-" + std::to_string(++counter);

    Json::Value envelope;
    envelope["schema_version"] = "1.0";
    envelope["request_id"] = request_id;
    envelope["source"] = "coremind-sidecar";
    envelope["original_request"] = original_request;
    envelope["platform_context"]["services"] = services;

    std::string bridge_url = get_env_or("BRIDGE_URL", "http://127.0.0.1:8199");

    // 从 bridge_url 提取 origin（scheme://host:port），丢弃路径后缀。
    // 脚本传入的 bridge_url 可能带 /health 后缀（如 http://127.0.0.1:8199/health），
    // 但 Bridge 的 callback 端点是固定路径 /internal/coremind/v1/callback，不能拼接。
    static constexpr const char* kCallbackPath = "/internal/coremind/v1/callback";
    auto scheme_pos = bridge_url.find("://");
    auto auth_start = (scheme_pos == std::string::npos) ? 0 : scheme_pos + 3;
    auto path_start = bridge_url.find('/', auth_start);
    std::string origin = (path_start == std::string::npos)
                             ? bridge_url
                             : bridge_url.substr(0, path_start);

    httplib::Client cli(origin.c_str());
    cli.set_read_timeout(360, 0);
    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Request-Id", request_id);

    auto response = cli.Post(kCallbackPath, headers, json_dumps(envelope), "application/json");
    if (!response) {
        error_response(context, 502, "BRIDGE_UNREACHABLE");
        return CMFrm::COM::SYNC;
    }

    // 5. 返回 Bridge 的响应
    context->WriteStatueCode(static_cast<CMFrm::COM::HttpStatus>(response->status));
    context->WriteJSONContentType();
    context->WriteResponseBody(response->body);

    std::cout << "[sidecar_router] forwarded to bridge: request_id="
              << request_id << " status=" << response->status << std::endl;
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
