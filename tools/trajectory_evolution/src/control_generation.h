#pragma once

#include "integration_json.h"
#include "runtime_policy.pb.h"

namespace trajectory::evolution::integration {
void validate_control_learning(const Json& options);
Json control_generation_input(const v1::ShadowPolicySnapshot& snapshot,
                              const v1::RuntimeOptions& base, const Json& options);
v1::RuntimeOptions apply_control_draft(const v1::ShadowPolicySnapshot& snapshot,
                                     const v1::RuntimeOptions& base, const Json& options, const Json& draft);
bool generated_controls_authorized(const v1::RuntimePolicy& candidate,
                                   const v1::RuntimePolicy& current, const Json& options);
v1::RuntimeOptions without_generated_controls(v1::RuntimeOptions options);
}  // namespace trajectory::evolution::integration
