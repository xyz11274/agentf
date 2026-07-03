#include "resolve_handler.h"

#include <cstdlib>
#include <sstream>

ResolveResult resolve_service(const std::string& service_name) {
    ResolveResult result;
    result.service_name = service_name;
    const char* configured = std::getenv("COREMIND_STUB_SERVICES");
    if (configured != nullptr) {
        std::istringstream entries(configured);
        std::string entry;
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
                return result;
            }
        }
    }
    result.error = "CoreMind service discovery is disabled in this build";
    result.success = false;
    return result;
}
