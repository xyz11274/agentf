#include "control_generation.h"
#include "local_support.h"

#include <set>
#include <openssl/sha.h>

namespace trajectory::evolution::integration {
namespace {
std::string rule_id(const Json& item) {
  const auto text = item.dump(); unsigned char bytes[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), bytes);
  std::string id = "learned-"; const char* hex = "0123456789abcdef";
  for(unsigned i = 0; i < 16; ++i) { id += hex[bytes[i] >> 4]; id += hex[bytes[i] & 15]; }
  return id;
}
const v1::ShadowExperience& source(const v1::ShadowPolicySnapshot& snapshot,
    const v1::RuntimeOptions& base, const std::string& id) {
  for(const auto& entry : snapshot.experiences()) if(entry.memory_id() == id)
    for(const auto& approval : base.approved_experiences())
      if(approval.memory_id() == id && approval.content_revision() == entry.content_revision()) return entry;
  throw Error("control_source_not_approved");
}
bool tighter(unsigned proposed, unsigned base, unsigned minimum) {
  return proposed == base || (proposed >= minimum && (!base || proposed <= base));
}
}

void validate_control_learning(const Json& options) {
  if(options.null()) return;
  keys(options, {"enabled", "prompt_file", "actions", "max_rules", "min_iterations", "min_no_progress", "max_attempts", "retry_backoff_ms"});
  require(options.num("max_rules", 4) > 0 && options.num("max_rules", 4) <= 16
      && options.num("min_iterations", 2) >= 1 && options.num("min_iterations", 2) <= 10000
      && options.num("min_no_progress", 1) >= 1 && options.num("min_no_progress", 1) <= 10000
      && options.num("max_attempts", 2) >= 1 && options.num("max_attempts", 2) <= 3
      && options.num("retry_backoff_ms", 60000) >= 1000 && options.num("retry_backoff_ms", 60000) <= 86400000,
      "control_learning_limits_invalid");
  if(options.flag("enabled", false)) require(!options.str("prompt_file").empty(), "control_prompt_required");
  if(options.has("actions")) {
    require(options["actions"].object() && options["actions"].fields().size() <= 64, "control_actions_invalid");
    for(const auto& action : options["actions"].fields()) {
      require(identifier(action.first) && action.second.array() && action.second.items().size() <= 32, "control_action_invalid");
      std::set<std::string> names;
      for(const auto& key : action.second.items()) require(identifier(key.text()) && names.insert(key.text()).second, "control_argument_invalid");
    }
  }
  validate_local_data(options);
}

Json control_generation_input(const v1::ShadowPolicySnapshot& snapshot,
    const v1::RuntimeOptions& base, const Json& options) {
  Json memories = Json::Array{};
  for(const auto& item : snapshot.experiences()) {
    bool allowed = false;
    for(const auto& approval : base.approved_experiences()) allowed |= approval.memory_id() == item.memory_id()
        && approval.content_revision() == item.content_revision();
    if(allowed) memories.push(Json{{"memory_id", item.memory_id()}, {"scope", item.target_scope()},
        {"condition", item.trigger_pattern()}, {"problem", item.observed_problem()}, {"experience", item.content()}});
  }
  const auto raw = json(base);
  Json limits = options; limits.fields().erase("prompt_file");
  return Json{{"memories", memories}, {"level", "L" + std::to_string(int(base.requested_level()) - int(v1::L0))},
      {"limits", limits}, {"base_action_rules", raw.has("action_rules") ? raw["action_rules"] : Json(Json::Array{})},
      {"base_loop_rules", raw.has("loop_rules") ? raw["loop_rules"] : Json(Json::Array{})}};
}

v1::RuntimeOptions without_generated_controls(v1::RuntimeOptions options) {
  auto actions = options.action_rules(); options.clear_action_rules();
  for(const auto& rule : actions) if(!rule.has_source_experience()) *options.add_action_rules() = rule;
  auto loops = options.loop_rules(); options.clear_loop_rules();
  for(const auto& rule : loops) if(!rule.has_source_experience()) *options.add_loop_rules() = rule;
  return options;
}

v1::RuntimeOptions apply_control_draft(const v1::ShadowPolicySnapshot& snapshot,
    const v1::RuntimeOptions& base, const Json& options, const Json& draft) {
  validate_control_learning(options); require(options.flag("enabled", false), "control_learning_disabled");
  keys(draft, {"action_rules", "loop_rules"}); validate_local_data(draft);
  require(draft["action_rules"].array() && draft["loop_rules"].array()
      && draft["action_rules"].items().size() + draft["loop_rules"].items().size() <= options.num("max_rules", 4), "control_draft_limits_invalid");
  auto result = base;
  for(const auto& item : draft["action_rules"].items()) {
    keys(item, {"memory_id", "operation", "required_arguments", "reason"});
    require(base.requested_level() >= v1::L2 && options["actions"].has(item.str("operation")), "control_operation_not_authorized");
    const auto& experience = source(snapshot, base, item.str("memory_id"));
    auto* rule = result.add_action_rules(); rule->set_rule_id(rule_id(item)); rule->set_target_scope(experience.target_scope());
    rule->set_operation(item.str("operation")); rule->set_reason(item.str("reason"));
    require(item["required_arguments"].array() && !item["required_arguments"].items().empty(), "control_arguments_required");
    std::set<std::string> unique;
    for(const auto& key : item["required_arguments"].items()) {
      bool allowed = false;
      for(const auto& value : options["actions"][item.str("operation")].items()) allowed |= key.text() == value.text();
      require(allowed && unique.insert(key.text()).second, "control_argument_not_authorized"); rule->add_required_arguments(key.text());
    }
    rule->mutable_source_experience()->set_memory_id(experience.memory_id());
    rule->mutable_source_experience()->set_content_revision(experience.content_revision());
    rule->clear_rule_id(); rule->set_rule_id(rule_id(json(*rule)));
  }
  std::set<std::string> scopes;
  for(const auto& item : draft["loop_rules"].items()) {
    keys(item, {"memory_id", "max_iterations", "max_no_progress", "reason"});
    require(base.requested_level() >= v1::L3, "control_loop_level_required");
    const auto& experience = source(snapshot, base, item.str("memory_id"));
    require(scopes.insert(experience.target_scope()).second, "control_duplicate_loop_scope");
    const v1::LoopRule* original = nullptr;
    for(const auto& rule : base.loop_rules()) if(rule.target_scope() == experience.target_scope() && !rule.has_source_experience()) original = &rule;
    require(original, "control_base_loop_required");
    const auto iterations = item.num("max_iterations", original->max_iterations());
    const auto no_progress = item.num("max_no_progress", original->max_no_progress());
    require(iterations <= 10000 && no_progress <= 10000
        && tighter(iterations, original->max_iterations(), options.num("min_iterations", 2))
        && tighter(no_progress, original->max_no_progress(), options.num("min_no_progress", 1)), "control_loop_relaxes_limit");
    if(iterations == original->max_iterations() && no_progress == original->max_no_progress()) continue;
    auto* rule = result.add_loop_rules(); *rule = *original;
    rule->set_rule_id(rule_id(item)); rule->set_max_iterations(iterations); rule->set_max_no_progress(no_progress); rule->set_reason(item.str("reason"));
    rule->mutable_source_experience()->set_memory_id(experience.memory_id());
    rule->mutable_source_experience()->set_content_revision(experience.content_revision());
    rule->clear_rule_id(); rule->set_rule_id(rule_id(json(*rule)));
  }
  require(result.action_rules_size() <= 64 && result.loop_rules_size() <= 64, "control_rule_capacity_reached");
  return result;
}

bool generated_controls_authorized(const v1::RuntimePolicy& candidate,
    const v1::RuntimePolicy& current, const Json& options) {
  try {
    Json draft{{"action_rules", Json::Array{}}, {"loop_rules", Json::Array{}}};
    for(const auto& rule : candidate.options().action_rules()) if(rule.has_source_experience()) {
      require(!rule.deny(), "control_deny_not_authorized");
      const auto& entry = source(current.snapshot(), current.options(), rule.source_experience().memory_id());
      require(entry.content_revision() == rule.source_experience().content_revision() && entry.target_scope() == rule.target_scope(), "control_source_changed");
      Json arguments = Json::Array{}; for(const auto& key : rule.required_arguments()) arguments.push(key);
      draft["action_rules"].push(Json{{"memory_id", entry.memory_id()}, {"operation", rule.operation()},
          {"required_arguments", arguments}, {"reason", rule.reason()}});
    }
    for(const auto& rule : candidate.options().loop_rules()) if(rule.has_source_experience()) {
      const auto& entry = source(current.snapshot(), current.options(), rule.source_experience().memory_id());
      require(entry.content_revision() == rule.source_experience().content_revision() && entry.target_scope() == rule.target_scope(), "control_source_changed");
      draft["loop_rules"].push(Json{{"memory_id", entry.memory_id()}, {"max_iterations", std::size_t(rule.max_iterations())},
          {"max_no_progress", std::size_t(rule.max_no_progress())}, {"reason", rule.reason()}});
    }
    if(draft["action_rules"].items().empty() && draft["loop_rules"].items().empty()) return true;
    auto rebuilt = apply_control_draft(current.snapshot(), without_generated_controls(candidate.options()), options, draft);
    return json(rebuilt).dump() == json(candidate.options()).dump();
  } catch(...) { return false; }
}
}  // namespace trajectory::evolution::integration
