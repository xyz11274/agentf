#include "resolve_handler.h"

#include <iostream>
#include <vector>

// CoreMind headers — only available in Huawei CI/CD build environment
#include "microservice/microservice_manager.h"
#include "framework_common/utils/env_util.h"
#include "framework_common/utils/retry_util.h"

ResolveResult resolve_service(const std::string& service_name) {
    ResolveResult result;
    result.service_name = service_name;

    // ① Query CoreMind MicroserviceManager
    // 每次请求都重新发现，不跨请求缓存（与设计文档 §2.2 对齐）。
    // 早年从 HealthAgent http_client.cpp 继承的进程级缓存（g_instance_cache）已移除——
    // 该缓存永不失效，与"每次请求重新发现"承诺冲突，且 MicroserviceManager 本地 RPC
    // 足够快，不需要缓存层。
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
    }, CMFrm::Constants::RetryCount2, CMFrm::Constants::RetryBackOffSecond2);

    if (!ok || resp.instanceList.empty()) {
        result.error = "QueryInstanceList failed or returned empty";
        result.success = false;
        return result;
    }

    std::vector<ServiceInstance> instances;
    for (const auto& inst : resp.instanceList) {
        // ParseEndpoint 在当前 SDK 版本中不存在，内联解析 "ip:port"
        auto ep = inst.endpoint;
        auto colon = ep.find(':');
        std::string ip = (colon != std::string::npos) ? ep.substr(0, colon) : ep;
        std::string port = (colon != std::string::npos) ? ep.substr(colon + 1) : "";
        int port_num = 0;
        try {
            port_num = std::stoi(port);
        } catch (const std::exception& e) {
            std::cerr << "[coremind_bridge] skip malformed endpoint: "
                      << service_name << " → " << inst.endpoint
                      << " (" << e.what() << ")" << std::endl;
            continue;
        }
        ServiceInstance si;
        si.ip = ip;
        si.port = port_num;
        instances.push_back(si);

        std::cout << "[coremind_bridge] resolved: " << service_name
                  << " → " << ip << ":" << port << std::endl;
    }

    result.instances = instances;
    result.success = true;
    return result;
}
