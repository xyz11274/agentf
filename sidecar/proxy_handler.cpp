/**
 * proxy_handler.cpp — CoreMind 平台服务代理端点（memory/rag/context）。
 *
 * 生产模式（COREMIND_ENABLED=ON）：通过 CoreMind HTTP/2 客户端 + mTLS 调用后端服务。
 * Stub 模式（COREMIND_ENABLED=OFF）：返回 mock 响应，供本地开发和 E2E 测试使用。
 *
 * 路由表：
 *   POST /proxy/memory/query  → KMMService（查询记忆）
 *   POST /proxy/memory/store  → KMMService（存储记忆）
 *   POST /proxy/rag/search    → GaussClient（RAG 检索）
 *   POST /proxy/context/add   → ContextManager（添加上下文）
 */

#include "proxy_handler.h"

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <json/json.h>
#include <sstream>
#include <string>

#ifdef COREMIND_ENABLED
#include <framework_com/http_client_factory.h>
#include "resolve_handler.h"
#endif

// ── JSON 辅助 ──

static Json::StreamWriterBuilder json_writer() {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    return w;
}

static std::string json_dumps(const Json::Value& val) {
    return Json::writeString(json_writer(), val);
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

static std::string get_env_or(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : fallback;
}

static std::string current_timestamp_ms() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms);
}

// ── httplib 模式用的 set_json_response ──

static void set_json_response(httplib::Response& res, const Json::Value& json, int status = 200) {
    res.status = status;
    res.set_content(json_dumps(json), "application/json");
}

// ── 通用代理 POST（通过 CoreMind HTTP/2 客户端） ──

#ifdef COREMIND_ENABLED
static Json::Value proxy_via_coremind(const std::string& service_name,
                                       const std::string& path,
                                       const Json::Value& body) {
    Json::Value result;
    result["status"] = "error";
    result["error"] = "unavailable";

    // 服务发现
    auto snap = resolve_service(service_name);
    if (!snap.success || snap.instances.empty()) {
        std::cerr << "[proxy] " << service_name << " not resolved" << std::endl;
        result["error"] = "SERVICE_UNREACHABLE";
        return result;
    }

    auto& inst = snap.instances[0];
    std::string target = "https://" + inst.ip + ":" + std::to_string(inst.port) + path;
    std::cout << "[proxy] forwarding to " << target << std::endl;

    // 使用 CoreMind HTTP/2 客户端
    auto httpFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http2Client = httpFactory->GetHttp2Client("default");
    if (!http2Client) {
        result["error"] = "HTTP2_CLIENT_UNAVAILABLE";
        return result;
    }

    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->SetMethod(CMFrm::COM::POST);
    req->SetReqUrl(target);
    req->AddHeader("Content-Type", "application/json");
    req->AddBody(json_dumps(body));
    req->SetTimeoutSecond(30);

    auto httpResult = http2Client->Send(req);
    if (!httpResult || !httpResult->Is2XXStatusCode()) {
        result["error"] = "UPSTREAM_ERROR";
        return result;
    }

    std::string resp_body = httpResult->GetResponseBody();
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(resp_body);
    if (Json::parseFromStream(reader, input, &result, &errors)) {
        result["status"] = "ok";
    } else {
        result["raw_body"] = resp_body;
        result["status"] = "ok";
    }
    return result;
}
#else
static Json::Value proxy_stub_response(const std::string& service) {
    Json::Value stub;
    stub["status"] = "stub";
    stub["service"] = service;
    stub["message"] = "Stub mode: no CoreMind service available";
    stub["data"] = Json::arrayValue;
    return stub;
}
#endif

// ── HTTP POST handler 实现 ──

void handle_proxy_memory_query(const httplib::Request& req, httplib::Response& res) {
    std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path
              << " | body=\n" << pretty_json(req.body) << std::endl;
#ifdef COREMIND_ENABLED
    Json::Value body;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(req.body);
    if (!Json::parseFromStream(reader, input, &body, &errors)) {
        { Json::Value v; v["status"] = "error"; v["error"] = "invalid_json"; set_json_response(res, v, 400); }
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 400 | error=invalid_json" << std::endl;
        return;
    }
    auto result = proxy_via_coremind("KMMService", "/memory/query", body);
    set_json_response(res, result);
#else
    set_json_response(res, proxy_stub_response("KMMService(memory/query)"));
#endif
    std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
              << " " << res.status << std::endl;
}

void handle_proxy_memory_store(const httplib::Request& req, httplib::Response& res) {
    std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path
              << " | body=\n" << pretty_json(req.body) << std::endl;
#ifdef COREMIND_ENABLED
    Json::Value body;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(req.body);
    if (!Json::parseFromStream(reader, input, &body, &errors)) {
        { Json::Value v; v["status"] = "error"; v["error"] = "invalid_json"; set_json_response(res, v, 400); }
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 400 | error=invalid_json" << std::endl;
        return;
    }
    auto result = proxy_via_coremind("KMMService", "/memory/store", body);
    set_json_response(res, result);
#else
    set_json_response(res, proxy_stub_response("KMMService(memory/store)"));
#endif
    std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
              << " " << res.status << std::endl;
}

void handle_proxy_rag_search(const httplib::Request& req, httplib::Response& res) {
    std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path
              << " | body=\n" << pretty_json(req.body) << std::endl;
#ifdef COREMIND_ENABLED
    Json::Value body;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(req.body);
    if (!Json::parseFromStream(reader, input, &body, &errors)) {
        { Json::Value v; v["status"] = "error"; v["error"] = "invalid_json"; set_json_response(res, v, 400); }
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 400 | error=invalid_json" << std::endl;
        return;
    }
    // GaussClient 是 C++ SDK，暂用 stub 占位
    // TODO: 接入 GaussClient::Instance()->Search()
    { Json::Value v; v["status"] = "stub"; v["service"] = "GaussClient(rag/search)"; set_json_response(res, v); }
#else
    set_json_response(res, proxy_stub_response("GaussClient(rag/search)"));
#endif
    std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
              << " " << res.status << std::endl;
}

void handle_proxy_context_add(const httplib::Request& req, httplib::Response& res) {
    std::cerr << "[coremind_bridge] REQ " << req.method << " " << req.path
              << " | body=\n" << pretty_json(req.body) << std::endl;
#ifdef COREMIND_ENABLED
    Json::Value body;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(req.body);
    if (!Json::parseFromStream(reader, input, &body, &errors)) {
        { Json::Value v; v["status"] = "error"; v["error"] = "invalid_json"; set_json_response(res, v, 400); }
        std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
                  << " 400 | error=invalid_json" << std::endl;
        return;
    }
    // ContextManager 是 C++ SDK，暂用 stub 占位
    // TODO: 接入 ContextManager::AddContext()
    { Json::Value v; v["status"] = "stub"; v["service"] = "ContextManager(context/add)"; set_json_response(res, v); }
#else
    set_json_response(res, proxy_stub_response("ContextManager(context/add)"));
#endif
    std::cerr << "[coremind_bridge] RES " << req.method << " " << req.path
              << " " << res.status << std::endl;
}
