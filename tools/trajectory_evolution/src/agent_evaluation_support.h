#pragma once

#include "trajectory_evolution/agent_evaluation.h"
#include "trajectory_evolution/task_evaluation.h"
#include "integration_json.h"

namespace trajectory::evolution::integration {

// Called once per run from a cached publication view; no disk/model/network I/O.
inline void apply_agent_version(Json& run, const Json& config, const Json& publications,
                                const v1::RuntimePolicy* current = nullptr) {
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
    try {
      require(artifact.str("level") == requested && current
          && artifact.str("user_id") == run["identity"].str("user_id"), "offline_policy_owner_or_level_mismatch");
      const auto frozen = bind_runtime_policy(proto<v1::RuntimePolicy>(artifact["policy"]), proto<v1::Identity>(run["identity"]));
      // Loading the trusted bundle grants its frozen source memories, not new
      // capabilities or looser deployment limits. No original memory DB needed.
      auto options = current->options();
      *options.mutable_approved_experiences() = frozen.options().approved_experiences();
      require(json(frozen.snapshot().options()).dump() == json(current->snapshot().options()).dump(), "offline_policy_scope_changed");
      const auto authorized = compile_runtime_policy(frozen.snapshot(), options);
      require(published_policy_compatible(frozen, authorized, config["control_learning"].dump()), "offline_policy_limits_changed");
    } catch(...) {
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
  if(artifact["policy"].object())
    run["policy"] = json(bind_runtime_policy(proto<v1::RuntimePolicy>(artifact["policy"]), proto<v1::Identity>(run["identity"])));
  run["agent_version_id"] = agent_version_id(artifact.dump());
  run["policy_gate"] = test ? "evaluation_instance" : offline ? "offline_version" : "published";
  if(run["events"].array() && !run["events"].items().empty())
    run["events"].items().front()["attributes"]["agent_version_id"] = run["agent_version_id"];
  if(test && run["events"].array() && !run["events"].items().empty())
    run["events"].items().front()["attributes"]["evaluation_trial_id"] = run["evaluation_trial_id"];
}

}  // namespace trajectory::evolution::integration
