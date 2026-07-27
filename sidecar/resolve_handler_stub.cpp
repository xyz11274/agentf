#include "resolve_handler.h"

#include <cstdlib>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

ResolveResult resolve_service(const std::string& service_name) {
    ResolveResult result;
    result.service_name = service_name;

    // 模拟重试延迟：如果设置了 COREMIND_STUB_RETRY_DELAY_MS，在返回结果前等待
    // 指定毫秒数，模拟 CMFrm::Utils::Retry 的重试退避行为。
    // 仅在至少有一个服务配置了该延迟时才生效，避免无配置时的不必要等待。
    const char* delay_env = std::getenv("COREMIND_STUB_RETRY_DELAY_MS");
    if (delay_env != nullptr) {
        try {
            int delay_ms = std::stoi(delay_env);
            if (delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                std::cout << "[coremind_bridge] stub: simulated retry delay "
                          << delay_ms << "ms" << std::endl;
            }
        } catch (const std::exception&) {
            // 非数字值静默忽略
        }
    }

    const char* configured = std::getenv("COREMIND_STUB_SERVICES");
    if (configured == nullptr) {
        result.error = "CoreMind service discovery is disabled in this build";
        result.success = false;
        return result;
    }

    // 解析格式：Name=ip:port,Name=ip:port,...
    // 支持同一服务名出现多次，每次追加一个实例（多实例支持）
    // 示例：
    //   IMInferScheduler=10.0.0.5:27321,IMInferScheduler=10.0.0.6:27321
    //   DMContextService=10.0.0.5:27203
    std::istringstream entries(configured);
    std::string entry;
    bool found = false;
    while (std::getline(entries, entry, ',')) {
        const auto equals = entry.find('=');
        const auto colon = entry.rfind(':');
        if (equals == std::string::npos || colon == std::string::npos || colon <= equals) {
            continue;
        }
        if (entry.substr(0, equals) == service_name) {
            result.instances.push_back({
                entry.substr(equals + 1, colon - equals - 1),
                std::stoi(entry.substr(colon + 1)),
            });
            result.success = true;
            found = true;
        }
    }

    if (!found) {
        result.error = "Service not found in stub configuration";
        result.success = false;
    }

    return result;
}