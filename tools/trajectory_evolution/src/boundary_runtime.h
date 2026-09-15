#pragma once

#include "integration_json.h"
#include "trajectory_evolution/policy_runtime.h"

namespace trajectory::evolution::integration {

// Framework-neutral boundary operations share persistence, authentication and
// evaluation with the compatibility API, but not its step/dispatch scheduler.
void validate_boundary_configuration(const Json& config);
Json describe_boundary_contract(const Json& config);
Json boundary_metrics(const Json& events);
Json boundary_trace(const Json& run);
std::string bounded_query_text(const Json& value, const Json& config, bool task);
Json boundary_begin_request(const Json& request, const std::string& run_id);
void initialize_boundary_run(Json& run, const Json& request);
Json dispatch_boundary(Json& run, const Json& request, const Json& config,
                       const PolicyRuntime* cached_policy = nullptr);

}  // namespace trajectory::evolution::integration
