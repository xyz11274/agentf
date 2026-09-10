#pragma once

#include "runtime_policy.pb.h"
#include "trajectory_evolution/shadow_runtime.h"

namespace trajectory::evolution {

v1::RuntimePolicy compile_runtime_policy(v1::ShadowPolicySnapshot snapshot,
                                         const v1::RuntimeOptions& options);

// Pure, immutable C++ control plane. The host owns execution and loop state.
class PolicyRuntime {
 public:
  explicit PolicyRuntime(v1::RuntimePolicy policy);
  const v1::RuntimePolicy& policy() const { return policy_; }
  v1::RuntimeDecision decide(const v1::RuntimeQuery& query) const;
  // Recomputes the decision; never trusts a caller-supplied plan as evidence.
  v1::RuntimeReceipt verify(const v1::RuntimeQuery& query,
                            const v1::RuntimeObservation& observation) const;
  // Per-call native request layout. Only binding fields are read; neither the
  // frozen policy nor its authorization, identity or context budget is changed.
  v1::RuntimeReceipt verify(const v1::RuntimeQuery& query,
                            const v1::RuntimeObservation& observation,
                            const v1::RuntimeOptions& request_binding) const;

 private:
  const v1::RuntimePolicy policy_;
  const ShadowRuntime shadow_;
  std::set<std::string> approved_ids_;
};

}  // namespace trajectory::evolution
