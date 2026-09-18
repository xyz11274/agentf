#pragma once

// 规则是否生成、是否获授权、是否实际命中，是三件需要分别验证的事。

#include "integration_json.h"
#include "runtime_policy.pb.h"
#include "evaluation_dataset.pb.h"

namespace trajectory::evolution::integration {
void validate_control_learning(const Json& options);
Json control_generation_input(const v1::ShadowPolicySnapshot& snapshot,
                              const v1::RuntimeOptions& base, const Json& options);
v1::RuntimeOptions apply_control_draft(const v1::ShadowPolicySnapshot& snapshot,
                                     const v1::RuntimeOptions& base, const Json& options, const Json& draft);
bool generated_controls_authorized(const v1::RuntimePolicy& candidate,
                                   const v1::RuntimePolicy& current, const Json& options);
v1::RuntimeOptions without_generated_controls(v1::RuntimeOptions options);
// Counts actual interventions, not rule presence or L1 injection receipts.
Json control_coverage(const Json& artifact, const dataset::v1::TaskEvaluationReport& report);
Json merge_control_coverage(Json coverage, const Json& previous);
}  // namespace trajectory::evolution::integration
