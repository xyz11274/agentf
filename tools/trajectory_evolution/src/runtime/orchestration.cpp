#include "trajectory_evolution/orchestration.h"
#include "agentflow/trajectory/hybrid_evaluator.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace trajectory::evolution {
namespace {
void require(bool value, const char* error) {
  if(!value) throw agentflow::trajectory::EvaluationError(error);
}
bool identifier(const std::string& value) {
  return !value.empty() && value.size() <= 512
      && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool terminal(const std::string& value) {
  return value == "succeeded" || value == "failed" || value == "cancelled"
      || value == "unknown" || value == "skipped";
}
std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  });
  return value;
}
bool matches_task(const v1::ScheduleRule& rule, const std::string& task) {
  if(rule.task_terms().empty() && rule.exclude_terms().empty()) return true;
  const auto text = lower_ascii(task);
  for(const auto& term : rule.task_terms()) if(text.find(lower_ascii(term)) == std::string::npos) return false;
  for(const auto& term : rule.exclude_terms()) if(text.find(lower_ascii(term)) != std::string::npos) return false;
  return true;
}
bool mutually_exclusive(const v1::ScheduleRule& left, const v1::ScheduleRule& right) {
  auto excludes_required = [](const v1::ScheduleRule& required, const v1::ScheduleRule& excluded) {
    for(const auto& term : required.task_terms()) for(const auto& forbidden : excluded.exclude_terms())
      if(lower_ascii(term).find(lower_ascii(forbidden)) != std::string::npos) return true;
    return false;
  };
  return excludes_required(left, right) || excludes_required(right, left);
}
bool acyclic(const v1::SchedulePlan& plan) {
  std::set<std::string> done;
  bool advanced = true;
  while(advanced) {
    advanced = false;
    for(const auto& action : plan.actions()) if(!done.count(action.action_id())) {
      bool ready = true;
      for(const auto& dependency : action.after()) ready &= done.count(dependency.action_id()) != 0;
      if(ready) { done.insert(action.action_id()); advanced = true; }
    }
  }
  return done.size() == std::size_t(plan.actions_size());
}
void validate_plan(const v1::SchedulePlan& plan) {
  require(identifier(plan.execution_id()) && plan.actions_size() > 0 && plan.actions_size() <= 128,
      "schedule_plan_invalid");
  std::set<std::string> ids;
  for(const auto& action : plan.actions())
    require(identifier(action.action_id()) && ids.insert(action.action_id()).second, "schedule_action_invalid");
  for(const auto& action : plan.actions()) {
    std::set<std::string> predecessors;
    require(action.after_size() <= 127, "schedule_dependency_limit");
    for(const auto& dependency : action.after())
      require(dependency.action_id() != action.action_id() && ids.count(dependency.action_id())
          && predecessors.insert(dependency.action_id()).second, "schedule_dependency_invalid");
  }
  require(acyclic(plan), "schedule_dependency_cycle");
}
std::map<std::string, std::string> states(const v1::ScheduleState& state) {
  validate_plan(state.plan());
  require(state.actions_size() == state.plan().actions_size(), "schedule_state_invalid");
  std::map<std::string, std::string> result;
  for(const auto& action : state.actions()) {
    require(action.status() == "waiting" || action.status() == "running" || terminal(action.status()),
        "schedule_status_invalid");
    require(result.emplace(action.action_id(), action.status()).second, "schedule_state_invalid");
  }
  for(const auto& action : state.plan().actions()) require(result.count(action.action_id()), "schedule_state_invalid");
  return result;
}
}

void validate_schedule_rules(const v1::RuntimeOptions& options) {
  require(options.schedule_rules_size() <= 64, "schedule_rule_limit");
  std::set<std::string> ids, guards;
  for(const auto& rule : options.schedule_rules()) {
    require(identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second
        && identifier(rule.target_scope()) && identifier(rule.before_operation()) && identifier(rule.after_operation())
        && !rule.reason().empty() && rule.reason().size() <= 1024, "schedule_rule_invalid");
    require(rule.task_terms_size() <= 8 && rule.exclude_terms_size() <= 8, "schedule_task_guard_invalid");
    require((rule.before_argument().empty() && rule.after_argument().empty())
        || (identifier(rule.before_argument()) && identifier(rule.after_argument())), "schedule_resource_guard_invalid");
    std::set<std::string> terms;
    for(const auto* list : {&rule.task_terms(), &rule.exclude_terms()}) for(const auto& term : *list)
      require(term.size() >= 2 && term.size() <= 128 && identifier(term)
          && terms.insert(lower_ascii(term)).second, "schedule_task_guard_invalid");
    std::vector<std::string> ordered_terms, ordered_exclusions;
    for(const auto& term : rule.task_terms()) ordered_terms.push_back(lower_ascii(term));
    for(const auto& term : rule.exclude_terms()) ordered_exclusions.push_back(lower_ascii(term));
    std::sort(ordered_terms.begin(), ordered_terms.end());
    std::sort(ordered_exclusions.begin(), ordered_exclusions.end());
    std::string guard = rule.target_scope() + "\n" + rule.before_operation() + "\n" + rule.after_operation();
    guard += "\nresource:" + rule.before_argument() + "\n" + rule.after_argument();
    for(const auto& term : ordered_terms) guard += "\n+" + term;
    for(const auto& term : ordered_exclusions) guard += "\n-" + term;
    require(guards.insert(guard).second, "schedule_rule_duplicate");
    require(!mutually_exclusive(rule, rule), "schedule_task_guard_invalid");
    if(rule.has_source_experience()) {
      bool approved = false;
      for(const auto& source : options.approved_experiences())
        approved |= source.memory_id() == rule.source_experience().memory_id()
            && source.content_revision() == rule.source_experience().content_revision();
      require(approved, "runtime_rule_source_not_approved");
    }
  }
  // Reject every cycle that could coexist with at least one rule in it.
  // Explicitly contradictory guards may describe alternate task workflows.
  for(const auto& focal : options.schedule_rules()) {
    v1::SchedulePlan plan;
    for(const auto& rule : options.schedule_rules()) {
      if(rule.target_scope() != focal.target_scope() || mutually_exclusive(focal, rule)
          || rule.before_operation() == rule.after_operation()) continue;
      for(const auto& name : {rule.before_operation(), rule.after_operation()}) {
        bool found = false;
        for(const auto& action : plan.actions()) found |= action.action_id() == name;
        if(!found) plan.add_actions()->set_action_id(name);
      }
      for(auto& action : *plan.mutable_actions()) if(action.action_id() == rule.after_operation())
        action.add_after()->set_action_id(rule.before_operation());
    }
    require(acyclic(plan), "schedule_rule_cycle");
  }
}

v1::SchedulePlan plan_schedule(const v1::RuntimeOptions& options, const v1::RuntimeQuery& query) {
  validate_schedule_rules(options);
  v1::SchedulePlan original;
  original.set_execution_id(query.execution_id()); original.set_status("unchanged");
  for(const auto& action : query.actions()) {
    require(identifier(action.operation()), "schedule_operation_invalid");
    auto* item = original.add_actions(); item->set_action_id(action.action_id());
    for(const auto& dependency : action.after()) {
      require(dependency.rule_id().empty(), "schedule_host_rule_id_invalid");
      *item->add_after() = dependency;
    }
  }
  validate_plan(original);
  if(options.requested_level() < v1::L2
      || std::find(options.host_capabilities().begin(), options.host_capabilities().end(), "schedule.dispatch")
          == options.host_capabilities().end()) return original;
  auto result = original;
  for(const auto& rule : options.schedule_rules()) {
    if(rule.target_scope() != query.advice_query().target_scope()
        || !matches_task(rule, query.advice_query().task())) continue;
    std::size_t before_count = 0, after_count = 0;
    for(const auto& action : query.actions()) {
      before_count += action.operation() == rule.before_operation();
      after_count += action.operation() == rule.after_operation();
    }
    const bool ambiguous = rule.has_source_experience() && rule.before_operation() != rule.after_operation()
        && (before_count > 1 || after_count > 1);
    bool changed = false;
    for(int i = 0; i < query.actions_size(); ++i) if(query.actions(i).operation() == rule.after_operation()) {
      auto* target = result.mutable_actions(i);
      for(int j = 0; j < query.actions_size(); ++j) {
        const auto& before = query.actions(j);
        if(before.operation() != rule.before_operation()
            || (rule.before_operation() == rule.after_operation() && j >= i)) continue;
        // 只用显式资源身份配对；不同产物与缺失参数不推断为同一工作流。
        const bool resource_bound = !rule.before_argument().empty();
        if(resource_bound) {
          const auto& outputs = before.arguments().fields();
          const auto& inputs = query.actions(i).arguments().fields();
          const auto output = outputs.find(rule.before_argument()), input = inputs.find(rule.after_argument());
          if(output == outputs.end() || input == inputs.end()
              || output->second.kind_case() != google::protobuf::Value::kStringValue
              || input->second.kind_case() != google::protobuf::Value::kStringValue
              || output->second.string_value().empty() || output->second.string_value().size() > 512
              || output->second.string_value().find("<redacted") != std::string::npos
              || output->second.string_value().find("[REDACTED") != std::string::npos
              || output->second.string_value().find("[redacted") != std::string::npos
              || output->second.string_value() != input->second.string_value()) continue;
        }
        // Operation-level learning cannot identify which of several independent
        // instances belong together. Refine known edges instead of guessing an
        // all-to-all dependency that lets one failure cancel unrelated work.
        if(ambiguous && !resource_bound && std::none_of(original.actions(i).after().begin(), original.actions(i).after().end(),
            [&](const auto& edge) { return edge.action_id() == before.action_id(); })) continue;
        v1::ActionDependency* edge = nullptr;
        for(auto& existing : *target->mutable_after()) if(existing.action_id() == before.action_id()) edge = &existing;
        if(!edge) { edge = target->add_after(); edge->set_action_id(before.action_id()); }
        else if(!rule.require_success() || edge->require_success()) continue;
        edge->set_require_success(edge->require_success() || rule.require_success());
        edge->set_rule_id(rule.rule_id()); changed = true;
      }
    }
    if(changed) result.add_rule_ids(rule.rule_id());
  }
  if(!acyclic(result)) {
    // A learned order may conflict with dependencies of this particular batch.
    // Keep the entire original plan; never silently delete a host dependency.
    original.set_status("conflict"); return original;
  }
  if(result.rule_ids_size()) result.set_status("scheduled");
  validate_plan(result);
  return result;
}

v1::ScheduleState begin_schedule(const v1::SchedulePlan& plan) {
  validate_plan(plan);
  v1::ScheduleState state; *state.mutable_plan() = plan;
  for(const auto& action : plan.actions()) {
    auto* item = state.add_actions(); item->set_action_id(action.action_id()); item->set_status("waiting");
  }
  return state;
}

std::vector<std::string> ready_schedule(const v1::ScheduleState& state) {
  const auto observed = states(state);
  std::vector<std::string> ready;
  for(const auto& action : state.plan().actions()) if(observed.at(action.action_id()) == "waiting") {
    bool allowed = true;
    for(const auto& dependency : action.after()) {
      const auto& status = observed.at(dependency.action_id());
      allowed &= dependency.require_success() ? status == "succeeded" : terminal(status);
    }
    if(allowed) ready.push_back(action.action_id());
  }
  return ready;
}

v1::ScheduleState advance_schedule(v1::ScheduleState state, const std::string& action_id, const std::string& status) {
  const auto before = states(state);
  const auto found = before.find(action_id);
  require(found != before.end() && (status == "running" || (terminal(status) && status != "skipped")),
      "schedule_transition_invalid");
  if(found->second == status) return state;
  if(status == "running") {
    const auto ready = ready_schedule(state);
    require(std::find(ready.begin(), ready.end(), action_id) != ready.end(), "schedule_predecessor_pending");
  } else require(found->second == "running" || (found->second == "waiting" && status == "cancelled"),
      "schedule_action_not_running");
  for(auto& item : *state.mutable_actions()) if(item.action_id() == action_id) item.set_status(status);
  // A failed prerequisite affects its success-dependent descendants only.
  bool changed = true;
  while(changed) {
    changed = false;
    const auto current = states(state);
    for(int i = 0; i < state.actions_size(); ++i) {
      auto* item = state.mutable_actions(i);
      if(item->status() != "waiting") continue;
      const v1::ScheduledAction* action = nullptr;
      for(const auto& candidate : state.plan().actions()) if(candidate.action_id() == item->action_id()) action = &candidate;
      for(const auto& dependency : action->after()) if(dependency.require_success()
          && terminal(current.at(dependency.action_id())) && current.at(dependency.action_id()) != "succeeded") {
        item->set_status("skipped"); changed = true; break;
      }
    }
  }
  return state;
}

bool schedule_finished(const v1::ScheduleState& state) {
  const auto observed = states(state);
  return std::all_of(observed.begin(), observed.end(), [](const auto& item) { return terminal(item.second); });
}

}  // namespace trajectory::evolution
