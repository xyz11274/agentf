#include "resolve_handler.h"

#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

// CoreMind headers — only available in Huawei CI/CD build environment
#include "microservice/microservice_manager.h"
#include "framework_common/utils/env_util.h"
#include "framework_common/utils/retry_util.h"

// ── Local cache (same pattern as HealthAgent http_client.cpp) ──
static std::shared_mutex g_cache_mutex;
static std::unordered_map<std::string, std::vector<ServiceInstance>> g_instance_cache;

ResolveResult resolve_service(const std::string& service_name) {
    ResolveResult result;
    result.service_name = service_name;

    // ① Check cache first
    {
        std::shared_lock lock(g_cache_mutex);
        auto it = g_instance_cache.find(service_name);
        if (it != g_instance_cache.end() && !it->second.empty()) {
            result.instances = it->second;
            result.success = true;
            return result;
        }
    }

    // ② Query CoreMind MicroserviceManager
    auto mgr = CMFrm::OM::MicroserviceManager::GetMicroserviceManager();
    if (!mgr) {
        result.error = "MicroserviceManager not available";
        result.success = false;
        return result;
    }

    CMFrm::OM::QueryInstanceReq req;
    req.serviceDiscovery = CMFrm::OM::BusinessServiceDiscovery;
    req.appId = CMFrm::Utils::GetEnv("APPID", "0");
    req.serviceName = service_name;
    req.filter = CMFrm::OM::All;

    CMFrm::OM::QueryInstanceResp resp;
    bool ok = CMFrm::Utils::Retry([&]() {
        return mgr->QueryInstanceList(req, resp);
    }, CMFrm::Utils::RetryCount2, CMFrm::Utils::RetryBackOffSecond2);

    if (!ok || resp.instanceList.empty()) {
        result.error = "QueryInstanceList failed or returned empty";
        result.success = false;
        return result;
    }

    std::vector<ServiceInstance> instances;
    for (const auto& inst : resp.instanceList) {
        auto [ip, port] = CMFrm::Utils::ParseEndpoint(inst.endpoint);
        ServiceInstance si;
        si.ip = ip;
        si.port = std::stoi(port);
        instances.push_back(si);

        std::cout << "[coremind_bridge] resolved: " << service_name
                  << " → " << ip << ":" << port << std::endl;
    }

    // ③ Update cache
    {
        std::unique_lock lock(g_cache_mutex);
        g_instance_cache[service_name] = instances;
    }

    result.instances = instances;
    result.success = true;
    return result;
}
