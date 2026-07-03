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
 *   cmake -B build -DPLATFORM_DIR=/path/to/coremind
 *   cmake --build build
 *
 * Run:
 *   ./coremind_bridge --listen 0.0.0.0:23767 \
 *                     --bridge-url http://127.0.0.1:8199
 */

#include <httplib.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <json/json.h>

#include "resolve_handler.h"

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
static void configure_server(httplib::Server& server, const std::string& bridge_url,
                             const std::string& internal_token);

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
    const std::string& internal_token,
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

    const auto target = parse_bridge_target(bridge_url);
    httplib::Client client(target.origin.c_str());
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + internal_token);
    headers.emplace("X-Request-Id", request_id);
    const auto response = client.Post(
        target.path_prefix + "/internal/coremind/v1/callback", headers,
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

int main(int argc, char* argv[]) {
    std::string listen_addr = "0.0.0.0:23767";
    std::string bridge_url = "http://127.0.0.1:8199";
    std::string internal_token = std::getenv("AISF_INTERNAL_TOKEN") != nullptr
        ? std::getenv("AISF_INTERNAL_TOKEN") : "";

    // ── Parse CLI args ──
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            listen_addr = argv[++i];
        } else if (arg == "--bridge-url" && i + 1 < argc) {
            bridge_url = argv[++i];
        } else {
            std::cerr << "Usage: coremind_bridge"
                      << " --listen <ip:port>"
                      << " --bridge-url <url>" << std::endl;
            return 1;
        }
    }
    if (internal_token.empty()) {
        std::cerr << "[coremind_bridge] AISF_INTERNAL_TOKEN is required" << std::endl;
        return 1;
    }

    ListenAddress listen;
    try {
        listen = parse_listen_address(listen_addr);
    } catch (const std::exception& error) {
        std::cerr << "[coremind_bridge] invalid --listen value: " << error.what() << std::endl;
        return 1;
    }

    httplib::Server svr;
    configure_server(svr, bridge_url, internal_token);

    std::cout << "[coremind_bridge] listening on " << listen_addr << std::endl;
    std::cout << "[coremind_bridge] forwarding to bridge: " << bridge_url << std::endl;

    if (!svr.bind_to_port(listen.host, listen.port)) {
        std::cerr << "[coremind_bridge] FAILED to bind " << listen_addr << std::endl;
        return 1;
    }
    svr.listen_after_bind();
    return 0;
}

static void configure_server(httplib::Server& svr, const std::string& bridge_url,
                             const std::string& internal_token) {

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
        json["capabilities"] = Json::Value(Json::arrayValue);
        json["capabilities"].append("outfit_consult");
        json["capabilities"].append("weather_query");
        json["capabilities"].append("occasion_guide");
        json["endpoint"] = "/coremind/systemagent/v1/callback";
        json["async_mode"] = true;
        set_json_response(res, json);
    });

    // ═══════════════════════════════════════════════
    //  3. Task callback → discover services and forward to Python Bridge
    // ═══════════════════════════════════════════════
    // Matches HealthAgent's USER_REQUEST_URL
    svr.Post("/coremind/systemagent/v1/callback",
             [bridge_url, internal_token](const httplib::Request& req, httplib::Response& res) {
        forward_to_bridge(bridge_url, internal_token, req, res);
    });

    // Alternate task entry paths (forward to adapter too)
    svr.Post("/health/v1/tasks",
             [bridge_url, internal_token](const httplib::Request& req, httplib::Response& res) {
        forward_to_bridge(bridge_url, internal_token, req, res);
    });

    svr.Post("/aisf/outfit/v1/tasks",
             [bridge_url, internal_token](const httplib::Request& req, httplib::Response& res) {
        forward_to_bridge(bridge_url, internal_token, req, res);
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
