#pragma once

#include <filesystem>
#include <set>

#include "shadow_policy.pb.h"
#include "hybrid_evaluation.pb.h"

namespace trajectory::evolution {

// The caller binds a trusted agent/user/run and explicitly authorizes scopes.
// Only single-user, non-conflicting experience candidates are compiled.
v1::ShadowPolicySnapshot compile_shadow_policy(
    const agentflow::trajectory::evaluation::EvolutionMemoryStore& memory,
    const v1::Identity& identity, const v1::ShadowOptions& options);
// Application publications have a separate, explicitly trusted input. Private
// ownership checks above are unchanged; a memory entry cannot opt into sharing.
v1::ShadowPolicySnapshot compile_shadow_policy(
    const agentflow::trajectory::evaluation::EvolutionMemoryStore& memory,
    const v1::Identity& identity, const v1::ShadowOptions& options,
    const v1::SharedExperienceStore& shared);
v1::ShadowPolicySnapshot compile_shadow_policy_file(
    const std::filesystem::path& memory_path,
    const v1::Identity& identity, const v1::ShadowOptions& options);

// Immutable, no I/O/model calls or shared mutable caches; concurrent queries safe.
v1::ShadowPolicySnapshot bind_shadow_policy(
    v1::ShadowPolicySnapshot snapshot, const v1::Identity& identity);

class ShadowRuntime {
 public:
  explicit ShadowRuntime(v1::ShadowPolicySnapshot snapshot);
  const v1::ShadowPolicySnapshot& snapshot() const { return snapshot_; }
  v1::AdviceResult query_advice(const v1::AdviceQuery& query) const;
  v1::AdviceResult query_advice(const v1::AdviceQuery& query,
                              const std::set<std::string>& approved_ids) const;

 private:
  v1::AdviceResult query(const v1::AdviceQuery& query,
                         const std::set<std::string>* approved_ids) const;
  const v1::ShadowPolicySnapshot snapshot_;
};

// A custom observation, not advice.applied, so it does not certify L1.
v1::TraceEvent shadow_trace_event(const v1::AdviceResult& result,
                                 const std::string& producer_id,
                                 std::uint64_t sequence);

}  // namespace trajectory::evolution
