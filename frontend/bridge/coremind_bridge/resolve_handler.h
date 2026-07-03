#ifndef COREMIND_BRIDGE_RESOLVE_HANDLER_H
#define COREMIND_BRIDGE_RESOLVE_HANDLER_H

#include <string>
#include <vector>

struct ServiceInstance {
    std::string ip;
    int port;
};

struct ResolveResult {
    std::string service_name;
    std::vector<ServiceInstance> instances;
    std::string error;
    bool success;
};

/// Resolve service instances via CoreMind service discovery.
/// Directly calls CMFrm::OM::MicroserviceManager.
ResolveResult resolve_service(const std::string& service_name);

#endif // COREMIND_BRIDGE_RESOLVE_HANDLER_H
