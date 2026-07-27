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
#include "sidecar_router.h"

// CoreMind framework headers — only available in Huawei build environment
#ifdef COREMIND_ENABLED
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
    Json::CharReaderBuilder reader_builder;
    Json::Value original_request;
    std::string errors;
    std::istringstream input(req.body);
    if (!Json::parseFromStream(reader_builder, input, &original_request, &errors) ||
        !original_request.isObject()) {
        res.status = 400;
        res.set_content(R"({"error":"invalid JSON request"})", "application/json");
        return;
    }

    Json::Value services(Json::objectValue);
    for (const auto& name : split_services(std::getenv("COREMIND_REQUIRED_SERVICES"))) {
        auto snapshot = service_snapshot(name);
        if (snapshot["status"].asString() != "resolved") {
            res.status = 503;
            res.set_content(R"({"error":"REQUIRED_SERVICE_DISCOVERY_FAILED"})",
                            "application/json");
            return;
        }
        services[name] = snapshot;
    }
    for (const auto& name : split_services(std::getenv("COREMIND_OPTIONAL_SERVICES"))) {
        services[name] = service_snapshot(name);
    }

    static std::atomic<unsigned long long> request_counter{0};
    auto request_id = req.get_header_value("X-Request-Id");
    if (request_id.empty()) {
        request_id = "sidecar-" + std::to_string(++request_counter);
    }
    Json::Value envelope;
    envelope["schema_version"] = "1.0";
    envelope["request_id"] = request_id;
    envelope["source"] = "coremind-sidecar";
    envelope["original_request"] = original_request;
    envelope["platform_context"]["services"] = services;

    // 从 bridge_url 提取 origin，丢弃路径后缀（如 /health），
    // callback 端点是固定路径 /internal/coremind/v1/callback
    const auto target = parse_bridge_target(bridge_url);
    httplib::Client client(target.origin.c_str());
    httplib::Headers headers;
    headers.emplace("X-Request-Id", request_id);
    static constexpr const char* kCallbackPath = "/internal/coremind/v1/callback";
    const auto response = client.Post(
        kCallbackPath, headers,
        Json::writeString(json_writer(), envelope), "application/json");
    if (!response) {
        res.status = 502;
        res.set_content(R"({"error":"BRIDGE_UNREACHABLE","status":"error"})",
                        "application/json");
        return;
    }

    res.status = response->status;
    const auto response_content_type = response->get_header_value("Content-Type");
    res.set_content(response->body,
                    response_content_type.empty() ? "application/octet-stream"
                                                  : response_content_type);
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
    const auto health_handler = [](const httplib::Request&, httplib::Response& res) {
        Json::Value json;
        json["status"] = "healthy";
        json["coremind"] = true;
        json["sidecar"] = true;
        set_json_response(res, json);
    };
    svr.Get("/health", health_handler);
    svr.Get("/healthcheck", health_handler);

    // ═══════════════════════════════════════════════
    //  2. Capability advertisement (handled locally)
    // ═══════════════════════════════════════════════
    svr.Get("/sysagent/v1/scheduleTaskAbility",
            [](const httplib::Request&, httplib::Response& res) {
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
        set_json_response(res, json, result.success ? 200 : 503);
    });

    svr.Post("/resolve/batch", [](const httplib::Request& req, httplib::Response& res) {
        Json::CharReaderBuilder reader_builder;
        Json::Value request_json;
        std::string errors;
        std::istringstream input(req.body);
        if (!Json::parseFromStream(reader_builder, input, &request_json, &errors) ||
            !request_json["services"].isArray()) {
            res.status = 400;
            res.set_content(R"({"status":"error","error":"services must be an array"})",
                            "application/json");
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
    });

    // ═══════════════════════════════════════════════
    //  5. Agent management stubs (handled locally)
    // ═══════════════════════════════════════════════
    svr.Post("/aisf/sysagent/v1/agentInstall",
             [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok","action":"install"})", "application/json");
    });

    svr.Post("/aisf/sysagent/v1/agentUpdate",
             [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok","action":"update"})", "application/json");
    });

    svr.Post("/aisf/sysagent/v1/agentUninstall",
             [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok","action":"uninstall"})", "application/json");
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
