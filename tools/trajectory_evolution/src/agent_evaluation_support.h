#pragma once

#include "trajectory_evolution/agent_evaluation.h"
#include "trajectory_evolution/task_evaluation.h"
#include "integration_json.h"

namespace trajectory::evolution::integration {

// Called once per run from a cached publication view; no disk/model/network I/O.
inline void apply_agent_version(Json& run, const Json& config, const Json& publications,
                                const v1::RuntimePolicy* current = nullptr,
                                const std::vector<std::string>& authorized_scopes = {}) {
  const auto requested = "L" + std::to_string(run.num("level", 0));
  Json artifact;
  const bool test = config.has("evaluation_version");
  const bool offline = config.has("offline_version");
  if(test) {
    artifact = config["evaluation_version"]["artifact"];
    require(artifact.str("level") == requested, "evaluation_runtime_level_mismatch");
    require(artifact.str("user_id") == run["identity"].str("user_id"), "evaluation_runtime_owner_mismatch");
    run["evaluation_trial_id"] = config["evaluation_version"]["trial_id"];
  } else if(offline) {
    artifact = config["offline_version"];
    if(artifact.str("level") != requested || artifact.str("user_id") != run["identity"].str("user_id")) {
      run["requested_level"] = requested; run["level"] = 0; run["policy_gate"] = "offline_policy_incompatible"; return;
    }
  } else if(config.has("policy_evaluation_file")) {
    run["requested_level"] = requested;
    artifact = publications[requested];
    try {
      if(requested != "L0" && (!current || !artifact["policy"].object()
          || !published_policy_compatible(proto<v1::RuntimePolicy>(artifact["policy"]), *current,
              config["control_learning"].dump()))) artifact = nullptr;
    } catch(...) { artifact = nullptr; }
    if(artifact.null()) {
      run["level"] = 0; run["policy_gate"] = "awaiting_evaluated_policy";
      return;
    }
  } else {
    if(current) run["policy"] = json(*current);
    return;
  }
  if(artifact["policy"].object()) {
    const auto frozen = bind_runtime_policy(proto<v1::RuntimePolicy>(artifact["policy"]), proto<v1::Identity>(run["identity"]));
    if(offline) {
      // The imported bundle owns its behavior. Check present-day authority,
      // not equality with historical retrieval, templates or request bindings.
      for(const auto& capability : frozen.options().host_capabilities()) {
        const std::string name = capability == "context.request" ? "model" : capability == "action.filter" ? "actions"
            : capability == "loop.decide" ? "loop" : "";
        bool found = false;
        for(const auto& declared : run["capabilities"].items()) found |= declared.text() == name;
        require(!name.empty() && found, "offline_policy_capability_missing");
      }
      for(const auto& target : frozen.snapshot().options().target_scopes()) {
        bool found = false;
        for(const auto& scope : authorized_scopes) found |= target == scope;
        require(found, "offline_policy_scope_missing");
      }
      const auto limits = proto<v1::RuntimeOptions>(config["runtime_options"]);
      require(!limits.max_context_bytes() || frozen.options().max_context_bytes() <= limits.max_context_bytes(),
          "offline_policy_limits_changed");
      for(const auto& guard : limits.action_rules()) {
        bool found = false;
        auto expected = json(guard); expected.fields().erase("rule_id"); expected.fields().erase("reason");
        for(const auto& rule : frozen.options().action_rules()) {
          auto actual = json(rule); actual.fields().erase("rule_id"); actual.fields().erase("reason");
          found |= actual.dump() == expected.dump();
        }
        require(found, "offline_policy_limits_changed");
      }
      for(const auto& guard : limits.loop_rules()) {
        bool bounded = false;
        for(const auto& rule : frozen.options().loop_rules()) {
          if(rule.target_scope() != guard.target_scope() || rule.has_source_experience()) continue;
          const bool rounds = !guard.max_iterations() || (rule.max_iterations() && rule.max_iterations() <= guard.max_iterations());
          const bool progress = !guard.max_no_progress() || (rule.max_no_progress() && rule.max_no_progress() <= guard.max_no_progress());
          const bool replans = !guard.max_no_progress() || rule.no_progress_decision() == "stop"
              || (guard.no_progress_decision() == "replan" && rule.max_replans() <= guard.max_replans());
          bounded |= rounds && progress && replans;
        }
        require(bounded, "offline_policy_limits_changed");
      }
    }
    run["policy"] = json(frozen);
  }
  run["agent_version_id"] = agent_version_id(artifact.dump());
  run["policy_gate"] = offline ? "offline_version" : test ? "evaluation_instance" : "published";
  if(run["events"].array() && !run["events"].items().empty())
    run["events"].items().front()["attributes"]["agent_version_id"] = run["agent_version_id"];
  if(test && run["events"].array() && !run["events"].items().empty())
    run["events"].items().front()["attributes"]["evaluation_trial_id"] = run["evaluation_trial_id"];
}

}  // namespace trajectory::evolution::integration
