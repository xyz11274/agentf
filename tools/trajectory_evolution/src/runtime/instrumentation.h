#pragma once

#include "integration_json.h"
#include "trajectory_evolution/policy_runtime.h"

namespace trajectory::evolution::integration {

// One JSON envelope for all language clients; no host scheduler or framework types.
Json point_request(const Json& request, const Json& config);
Json point_result(const Json& request, const Json& normalized, Json result);
Json point_fallback(const Json& request, const std::string& reason);
void validate_instrumentation(const Json& config);
// Optional host identifiers replace manual context storage across processes.
std::string point_run_key(const Json& request);
std::string point_call_key(const Json& request);
std::string point_request_key(const Json& request);
Json resolve_point_reference(const Json& request, Json& run, const std::string& run_id);
void record_point_reference(Json& run, const Json& request, const Json& result);
Json dispatch_referenced_point(Json& run, const Json& request, const Json& config,
    const PolicyRuntime* cached_policy = nullptr);
void abort_point_reference(Json& run, const Json& request, const Json& config,
    const PolicyRuntime* cached_policy = nullptr) noexcept;

}  // namespace trajectory::evolution::integration
