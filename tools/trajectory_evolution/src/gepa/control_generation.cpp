// 控制规则生成：把经验和观测到的宿主能力转成候选编排与循环规则。
// 可修改范围来自公开打点能力与动作描述，不从基线成绩凭空推导宿主能力。
// 生成后仍需校验作用域、动作参数和运行证据，规则存在不代表真实生效。

// 将反思候选转换为可评测的 L2/L3 控制规则，并统计实际干预。
#include "control_generation.h"
#include "local_support.h"
#include "trajectory_evolution/orchestration.h"

#include <algorithm>
#include <set>
#include <openssl/sha.h>

namespace trajectory::evolution::integration {
namespace {
constexpr const char *kSchemaOperation = "*";
constexpr const char *kSchemaRequiredNonempty = "$schema_required_nonempty";
constexpr const char *kSchemaPrimitive = "schema_required_nonempty";
constexpr const char *kRepeatPrimitive = "repeat_failed_operation";
constexpr const char *kTargetRepeatPrimitive = "repeat_failed_target";
constexpr const char *kTerminalPrimitive = "terminal_failure";
constexpr const char *kBudgetPrimitive = "operation_budget";

// 根据规则内容生成稳定标识，用于版本内去重、证据归因和执行回执匹配。
std::string rule_id(const Json &item) {
  const auto text = item.dump();
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(), bytes);
  std::string id = "learned-";
  const char *hex = "0123456789abcdef";
  for (unsigned i = 0; i < 16; ++i) {
    id += hex[bytes[i] >> 4];
    id += hex[bytes[i] & 15];
  }
  return id;
}

// 解析当前对象的来源，保留来源身份与后续引用所需的数据。
const v1::ShadowExperience &source(const v1::ShadowPolicySnapshot &snapshot,
                                   const v1::RuntimeOptions &base, const std::string &id) {
  for (const auto &entry : snapshot.experiences())
    if (entry.memory_id() == id)
      for (const auto &approval : base.approved_experiences())
        if (approval.memory_id() == id && approval.content_revision() == entry.content_revision())
          return entry;
  throw Error("control_source_not_approved");
}

// 选择更严格的有效边界，保持宿主原有硬上限不会被学习规则放宽。
bool tighter(unsigned proposed, unsigned base, unsigned minimum) {
  return proposed == base || (proposed >= minimum && (!base || proposed <= base));
}

// 检查宿主是否明确提供失败后重规划能力，不凭策略名称假定可执行。
bool failure_replan_available(const v1::RuntimeOptions &base,
                              const v1::LoopRule *selected = nullptr) {
  return std::any_of(base.loop_rules().begin(), base.loop_rules().end(), [&](const auto &rule) {
    if (selected && &rule != selected)
      return false;
    if (rule.has_source_experience() || rule.no_progress_decision() != "replan" ||
        !rule.max_replans())
      return false;
    // 激进失败计数且阈值为一时，基础“无进展”分支已会在首次重复失败后重规划；
    // 此时学习得到的 replan_on_failure 无法再改变该决策，不能将其视为有效变异。
    return !base.aggressive_controls() || !rule.max_no_progress() || rule.max_no_progress() > 1;
  });
}
}

// 验证控制学习配置与可用动作契约；不从评测基线臆造宿主能力。
void validate_control_learning(const Json &options) {
  if (options.null())
    return;
  keys(options, {"enabled", "prompt_file", "actions", "max_rules", "min_iterations",
                 "min_no_progress", "max_attempts", "retry_backoff_ms"});
  require(options.num("max_rules", 4) > 0 && options.num("max_rules", 4) <= 16 &&
              options.num("min_iterations", 2) >= 1 && options.num("min_iterations", 2) <= 10000 &&
              options.num("min_no_progress", 1) >= 1 &&
              options.num("min_no_progress", 1) <= 10000 && options.num("max_attempts", 2) >= 1 &&
              options.num("max_attempts", 2) <= 3 &&
              options.num("retry_backoff_ms", 60000) >= 1000 &&
              options.num("retry_backoff_ms", 60000) <= 86400000,
          "control_learning_limits_invalid");
  if (options.flag("enabled", false))
    require(!options.str("prompt_file").empty(), "control_prompt_required");
  if (options.has("actions")) {
    require(options["actions"].object() && options["actions"].fields().size() <= 64,
            "control_actions_invalid");
    for (const auto &action : options["actions"].fields()) {
      require(identifier(action.first) && action.second.array() &&
                  action.second.items().size() <= 32,
              "control_action_invalid");
      std::set<std::string> names;
      for (const auto &key : action.second.items())
        require(identifier(key.text()) && names.insert(key.text()).second,
                "control_argument_invalid");
    }
  }
  validate_local_data(options);
}

// 构建控制生成所需的最小输入：可用动作、允许修改的字段及相关反馈。
Json control_generation_input(const v1::ShadowPolicySnapshot &snapshot,
                              const v1::RuntimeOptions &base, const Json &options) {
  Json memories = Json::Array{};
  Json control_sources = Json::Array{};
  for (const auto &item : snapshot.experiences()) {
    bool allowed = false;
    for (const auto &approval : base.approved_experiences())
      allowed |= approval.memory_id() == item.memory_id() &&
                 approval.content_revision() == item.content_revision();
    if (!allowed)
      continue;
    Json value{{"memory_id", item.memory_id()},
               {"scope", item.target_scope()},
               {"condition", item.trigger_pattern()},
               {"problem", item.observed_problem()},
               {"experience", item.content()}};
    if (item.control_target_type().empty())
      memories.push(std::move(value));
    else {
      value["target_type"] = item.control_target_type();
      control_sources.push(std::move(value));
    }
  }
  const auto raw = json(base);
  Json limits = options;
  limits.fields().erase("prompt_file");
  Json input{
      {"memories", memories},
      {"level", "L" + std::to_string(int(base.requested_level()) - int(v1::L0))},
      {"aggressive_controls", base.aggressive_controls()},
      {"action_primitives",
       Json::Array{
           Json{{"primitive_id", kSchemaPrimitive},
                {"operation", kSchemaOperation},
                {"required_arguments", Json::Array{kSchemaRequiredNonempty}},
                {"effect", "reject_when_a_runtime_schema_required_argument_is_missing_or_empty"},
                {"limits", "Does not detect semantic placeholders or redundant tasks."}},
           Json{{"primitive_id", kRepeatPrimitive},
                {"effect",
                 "reject_an_operation_after_two_settled_failures_in_the_same_run_and_loop"},
                {"limits", "Specify the exact actions[].operation with observed "
                           "action_control.prior_failures >= 2; do not use an actor name, argument "
                           "action or wildcard. Other operations and pending branches continue. "
                           "An empty successful query is not a settled failure."}},
           Json{{"primitive_id", kTargetRepeatPrimitive},
                {"effect",
                 "reject_retries_of_the_same_explicit_target_across_executions_within_one_run"},
                {"limits", "Use an observed exact operation, 1-4 scalar failure_key_arguments "
                           "identifying the target (not an entire prompt), and max_failed_attempts "
                           "1-4. Different targets, missing keys and concurrent initial attempts "
                           "remain independent. Success clears the failure streak."}}}},
      {"matching", "New controls use scope and executable predicates, independent of L1 retrieval. "
                   "Loop limits apply to each execution in the scope."},
      {"loop_conditions",
       Json{{"error_codes", "Optional exact structured failure codes observed in the same "
                            "execution; any listed code matches."},
            {"replan_context", "Optional only for replan_on_failure with exactly one of "
                               "appworld_auth_401 or appworld_missing_access_token and at most "
                               "one authorized replan: copy the "
                               "primary approved memory's experience text exactly, within the "
                               "unchanged context budget. Free-form instructions are rejected."},
            {"after_action_rejection",
             "Optional true: requires a verified settled L2 rejection in this execution. "
             "Successful progress suppresses stale conditions; pending branches continue."},
            {"require_finalization",
             "Optional true: only applies at a boundary with explicit answer-only finalization "
             "support and a loaded finish template. Required when a cap replaces the reviewed "
             "final answer."},
            {"combination", "All specified conditions must match. Narrowed recovery budgets need "
                            "the observed condition, not only a reason describing it."}}},
      {"limits", limits},
      {"base_action_rules", raw.has("action_rules") ? raw["action_rules"] : Json(Json::Array{})},
      {"base_loop_rules", raw.has("loop_rules") ? raw["loop_rules"] : Json(Json::Array{})}};
  if (!control_sources.items().empty())
    input["control_sources"] = std::move(control_sources);
  input["action_primitives"].push(Json{
      {"primitive_id", kTerminalPrimitive},
      {"effect", "reject_after_an_observed_structured_terminal_error_in_the_same_operation_and_"
                 "configuration"},
      {"limits",
       "Use an exact observed operation and terminal_error_codes copied from structured failures. "
       "error_scope_arguments identify provider/model/configuration, never output paths or "
       "prompts. Missing optional configuration denotes the same default. Transient network "
       "failures are not terminal. Other operations/configurations remain independent."}});
  if (base.requested_level() >= v1::L3 && base.loop_rules_size()) {
    input["action_primitives"].push(
        Json{{"primitive_id", kBudgetPrimitive},
             {"effect", "bound_settled_attempt_rounds_for_an_exact_operation_in_one_run_including_"
                        "successful_noop_delegations"},
             {"limits", "Use observed operation, max_attempts 1-16, optional 1-4 stable scalar "
                        "attempt_key_arguments. Empty keys explicitly share a per-operation budget "
                        "within this run. Parallel first attempts count as one round. This is a "
                        "learned budget, not proof of task completion; retain only if task quality "
                        "is preserved. Never key by free-form prompt."}});
    input["loop_primitives"] = Json::Array{};
    input["loop_primitives"].push(
        Json{{"primitive_id", "bounded_no_progress"},
             {"effect", "tighten_max_no_progress_from_observed_no_progress_in_the_same_execution"},
             {"limits", "Requires an observed counter at or above the proposed threshold. Pending "
                        "actions remain protected."}});
    // 保守模式也可研究已评审的浪费后缀，但必须限定 actor 并保留宿主收尾。
    // 这不启用全局激进模式，也不把参数变化直接判成“没有进展”。
    input["loop_primitives"].push(
        Json{{"primitive_id", "bounded_iteration_cap"},
             {"effect", "lower_max_iterations_for_the_observed_actor"},
             {"limits", "Requires a reviewed waste suffix, preserves all reviewed necessary work "
                        "and task quality in evaluation. When aggressive_controls is false, "
                        "actor_id and require_finalization=true are mandatory and the observed "
                        "host must support can_finalize. Pending actions remain protected."}});
    if (failure_replan_available(base))
      input["loop_primitives"].push(Json{
          {"primitive_id", "replan_on_failure"},
          {"effect", "bounded_replan_after_a_repeated_structured_failure_in_the_same_execution"},
          {"limits", "Uses repeated_failure_steps from the previous control step; the first "
                     "failure does not trigger it. Pending actions and the base replan budget are "
                     "respected. Never stops solely on failure."}});
    if (std::any_of(base.loop_rules().begin(), base.loop_rules().end(), [](const auto &rule) {
          return !rule.has_source_experience() && rule.no_progress_decision() == "replan" &&
                 rule.max_replans();
        }))
      input["loop_primitives"].push(
          Json{{"primitive_id", "bounded_recovery_budget"},
               {"effect", "lower_max_replans_for_the_same_actor_and_execution"},
               {"limits", "Zero stops at the existing no-progress threshold instead of another "
                          "replan. Requires observed no-progress or ineffective replanning; never "
                          "stops on a first failure alone. Pending branches remain protected."}});
  }
  if (std::find(base.host_capabilities().begin(), base.host_capabilities().end(),
                "schedule.dispatch") != base.host_capabilities().end()) {
    input["schedule_primitives"] = Json::Array{Json{
        {"primitive_id", "order_operations"},
        {"effect", "wait_for_before_operation_completion_before_dispatching_after_operation_in_the_"
                   "same_batch"},
        {"limits", "只调整宿主实际提交的同一批次，不向模型规划输入追加提示或补充遗漏操作。"
                   "editable_bindings 是可选操作，不是已生效规则；当前规则以父组件和 "
                   "base_schedule_rules 为准，宿主依赖以 schedule_plans 为准。无边默认并行；"
                   "add 增加先后依赖，remove 只能删除父组件中真实存在的生成边，不能用同条件"
                   "双向 add 表示并行。使用可复用的 task_terms 和可选 exclude_terms，按当前 "
                   "Run 任务文本本地匹配。同名操作按宿主批次原顺序串行不同实例。保留宿主依赖；"
                   "require_success=true 在前置失败时仅跳过依赖分支，false 只等待完成。"
                   "无关操作保持并行；候选绑定、查询或预检命中不是实际调度应用回执。"}}};
    input["base_schedule_rules"] =
        raw.has("schedule_rules") ? raw["schedule_rules"] : Json(Json::Array{});
    input["schedule_primitives"].items()[0]["resource_condition"] =
        "Instead of task_terms, before_argument/after_argument may select equal nonempty resource "
        "strings shown in schedule_plans.actions.arguments, including the component-derived "
        "__evolution_resource (a unique shared explicit file path). This pairs independent "
        "producer/consumer instances across task languages. Missing or unequal resources do not "
        "match; never use full prompt text as a resource. Shared resources alone do not prove a "
        "dependency. Both actions must exist in the same batch.";
  }
  return input;
}

// 移除本轮模型生成的控制部分，保留其余策略内容，供替换或消融比较。
v1::RuntimeOptions without_generated_controls(v1::RuntimeOptions options) {
  auto actions = options.action_rules();
  options.clear_action_rules();
  for (const auto &rule : actions)
    if (!rule.has_source_experience())
      *options.add_action_rules() = rule;
  auto loops = options.loop_rules();
  options.clear_loop_rules();
  for (const auto &rule : loops)
    if (!rule.has_source_experience())
      *options.add_loop_rules() = rule;
  auto schedules = options.schedule_rules();
  options.clear_schedule_rules();
  for (const auto &rule : schedules)
    if (!rule.has_source_experience())
      *options.add_schedule_rules() = rule;
  return options;
}

// 把经过验证的控制草案写入候选策略；不改变源策略对象的既有身份约定。
v1::RuntimeOptions apply_control_draft(const v1::ShadowPolicySnapshot &snapshot,
                                       const v1::RuntimeOptions &base, const Json &options,
                                       const Json &draft) {
  validate_control_learning(options);
  require(options.flag("enabled", false), "control_learning_disabled");
  keys(draft, {"action_rules", "loop_rules", "schedule_rules"});
  validate_local_data(draft);
  const auto schedules =
      draft.has("schedule_rules") ? draft["schedule_rules"] : Json(Json::Array{});
  require(draft["action_rules"].array() && draft["loop_rules"].array() && schedules.array() &&
              draft["action_rules"].items().size() + draft["loop_rules"].items().size() +
                      schedules.items().size() <=
                  options.num("max_rules", 4),
          "control_draft_limits_invalid");
  auto result = base;
  for (const auto &item : draft["action_rules"].items()) {
    keys(item,
         {"memory_id", "operation", "required_arguments", "primitive_id", "reason", "match_mode",
          "max_failed_attempts", "failure_key_arguments", "terminal_error_codes",
          "error_scope_arguments", "max_attempts", "attempt_key_arguments"});
    const auto primitive = item.str("primitive_id");
    const auto terminal_codes =
        item.has("terminal_error_codes") ? item["terminal_error_codes"] : Json(Json::Array{});
    const auto error_keys =
        item.has("error_scope_arguments") ? item["error_scope_arguments"] : Json(Json::Array{});
    const bool terminal_rule =
        (primitive.empty() || primitive == kTerminalPrimitive) &&
        identifier(item.str("operation")) && item.str("operation") != kSchemaOperation &&
        terminal_codes.array() && !terminal_codes.items().empty() &&
        terminal_codes.items().size() <= 8 && error_keys.array() &&
        error_keys.items().size() <= 4 &&
        (!item.has("required_arguments") ||
         (item["required_arguments"].array() && item["required_arguments"].items().empty())) &&
        item.num("max_failed_attempts", 1) == 1;
    const auto budget_keys =
        item.has("attempt_key_arguments") ? item["attempt_key_arguments"] : Json(Json::Array{});
    const bool budget_rule =
        (primitive.empty() || primitive == kBudgetPrimitive) && base.requested_level() >= v1::L3 &&
        identifier(item.str("operation")) && item.str("operation") != kSchemaOperation &&
        item.num("max_attempts", 0) >= 1 && item.num("max_attempts", 0) <= 16 &&
        budget_keys.array() && budget_keys.items().size() <= 4 &&
        item.num("max_failed_attempts", 0) == 0 &&
        (!item.has("required_arguments") ||
         (item["required_arguments"].array() && item["required_arguments"].items().empty()));
    require(budget_rule || (item.num("max_attempts", 0) == 0 && budget_keys.array() &&
                            budget_keys.items().empty()),
            "control_budget_rule_invalid");
    require(terminal_rule || (terminal_codes.array() && terminal_codes.items().empty() &&
                              error_keys.array() && error_keys.items().empty()),
            "control_terminal_rule_invalid");
    const bool primitive_rule =
        primitive == kSchemaPrimitive && !item.has("operation") && !item.has("required_arguments");
    // 旧原语允许省略操作名；新 GEPA 提案在搜索边界要求显式绑定真实操作。
    // 此处兼容旧草稿，同时保留窄规则，不能把显式操作编译成通配符。
    const bool repeat_rule =
        primitive == kRepeatPrimitive &&
        (!item.has("operation") ||
         (identifier(item.str("operation")) && item.str("operation") != kSchemaOperation)) &&
        (!item.has("required_arguments") ||
         (item["required_arguments"].array() && item["required_arguments"].items().empty()));
    const auto failure_keys =
        item.has("failure_key_arguments") ? item["failure_key_arguments"] : Json(Json::Array{});
    const bool target_repeat =
        (primitive.empty() || primitive == kTargetRepeatPrimitive) &&
        identifier(item.str("operation")) && item.str("operation") != kSchemaOperation &&
        failure_keys.array() && !failure_keys.items().empty() && failure_keys.items().size() <= 4 &&
        (!item.has("required_arguments") ||
         (item["required_arguments"].array() && item["required_arguments"].items().empty()));
    const auto operation =
        primitive_rule || (repeat_rule && !item.has("operation"))
            ? std::string(kSchemaOperation)
            : item.str("operation");
    const Json arguments = primitive_rule ? Json(Json::Array{kSchemaRequiredNonempty})
                           : repeat_rule || target_repeat || terminal_rule || budget_rule
                               ? Json(Json::Array{})
                               : item["required_arguments"];
    const bool schema_rule = operation == kSchemaOperation && arguments.array() &&
                             arguments.items().size() == 1 &&
                             arguments.items().front().text() == kSchemaRequiredNonempty;
    const auto max_failed =
        terminal_rule ? 1 : item.num("max_failed_attempts", repeat_rule || target_repeat ? 2 : 0);
    // 已编译规则回放时不保留 primitive_id；精确操作的失败预算也必须可原样重建。
    const bool repeat_authorized =
        (operation == kSchemaOperation || repeat_rule || target_repeat || terminal_rule ||
         (primitive.empty() && identifier(operation))) &&
        arguments.array() && arguments.items().empty() &&
        max_failed >= (target_repeat || terminal_rule ? 1 : 2) && max_failed <= 4;
    const bool authorized =
        (primitive.empty() || primitive_rule || repeat_rule || target_repeat || terminal_rule ||
         budget_rule) &&
        (schema_rule || repeat_authorized || budget_rule || options["actions"].has(operation));
    require(failure_keys.array() && (failure_keys.items().empty() || target_repeat),
            "control_argument_not_authorized");
    require(base.requested_level() >= v1::L2, "control_operation_not_authorized");
    require(authorized, "control_operation_not_authorized");
    const auto memory_id = item.str("memory_id");
    const auto &experience = source(snapshot, base, memory_id);
    auto *rule = result.add_action_rules();
    rule->set_rule_id(rule_id(item));
    rule->set_target_scope(experience.target_scope());
    rule->set_operation(operation);
    rule->set_reason(item.str("reason"));
    rule->set_max_failed_attempts(max_failed);
    if (budget_rule) {
      require(!target_repeat && !terminal_rule, "control_budget_rule_invalid");
      rule->set_max_attempts(item.num("max_attempts", 0));
      std::set<std::string> seen;
      for (const auto &key : budget_keys.items()) {
        require(identifier(key.text()) && seen.insert(key.text()).second,
                "control_budget_rule_invalid");
        rule->add_attempt_key_arguments(key.text());
      }
    }
    if (terminal_rule) {
      std::set<std::string> seen_codes, seen_keys;
      for (const auto &code : terminal_codes.items()) {
        require(identifier(code.text()) && code.text().size() <= 128 &&
                    seen_codes.insert(code.text()).second,
                "control_terminal_rule_invalid");
        rule->add_terminal_error_codes(code.text());
      }
      for (const auto &key : error_keys.items()) {
        require(identifier(key.text()) && seen_keys.insert(key.text()).second,
                "control_terminal_rule_invalid");
        rule->add_error_scope_arguments(key.text());
      }
    }
    std::set<std::string> target_keys;
    for (const auto &key : failure_keys.items()) {
      require(identifier(key.text()) && target_keys.insert(key.text()).second,
              "control_argument_not_authorized");
    }
    for (const auto &key : target_keys)
      rule->add_failure_key_arguments(key);
    rule->set_match_mode(item.str("match_mode", "predicate"));
    require(rule->match_mode().empty() || rule->match_mode() == "predicate",
            "runtime_rule_match_mode_invalid");
    require(arguments.array() && (!arguments.items().empty() || repeat_authorized || budget_rule),
            "control_arguments_required");
    std::set<std::string> unique;
    for (const auto &key : arguments.items()) {
      bool allowed = schema_rule && key.text() == kSchemaRequiredNonempty;
      if (!schema_rule)
        for (const auto &value : options["actions"][operation].items())
          allowed |= key.text() == value.text();
      require(allowed && unique.insert(key.text()).second, "control_argument_not_authorized");
      rule->add_required_arguments(key.text());
    }
    rule->mutable_source_experience()->set_memory_id(experience.memory_id());
    rule->mutable_source_experience()->set_content_revision(experience.content_revision());
    rule->clear_rule_id();
    rule->set_rule_id(rule_id(json(*rule)));
  }
  std::set<std::pair<std::string, std::string>> scopes;
  for (const auto &item : draft["loop_rules"].items()) {
    keys(item, {"memory_id", "max_iterations", "max_no_progress", "max_replans",
                "replan_on_failure", "reason", "match_mode", "actor_id", "error_codes",
                "after_action_rejection", "require_finalization", "replan_context"});
    require(base.requested_level() >= v1::L3, "control_loop_level_required");
    const auto memory_id = item.str("memory_id");
    const auto &experience = source(snapshot, base, memory_id);
    require(scopes.emplace(experience.target_scope(), item.str("actor_id")).second,
            "control_duplicate_loop_scope");
    const v1::LoopRule *original = nullptr;
    for (const auto &rule : base.loop_rules())
      if (rule.target_scope() == experience.target_scope() && !rule.has_source_experience() &&
          (rule.actor_id().empty() || rule.actor_id() == item.str("actor_id")) &&
          (!original || original->actor_id().empty()))
        original = &rule;
    require(original, "control_base_loop_required");
    const auto iterations = item.num("max_iterations", original->max_iterations());
    const auto no_progress = item.num("max_no_progress", original->max_no_progress());
    const auto replans = item.num("max_replans", original->max_replans());
    require(replans <= original->max_replans(), "control_loop_relaxes_limit");
    const auto failure_replan =
        item.flag("replan_on_failure", replans && original->replan_on_failure());
    require(!failure_replan || replans, "control_failure_replan_not_authorized");
    require(!failure_replan || original->replan_on_failure() ||
                failure_replan_available(base, original),
            "control_failure_replan_not_authorized");
    require(!original->replan_on_failure() || failure_replan || replans < original->max_replans(),
            "control_loop_relaxes_limit");
    const bool source_context = item.has("replan_context");
    if (source_context) {
      require(item["replan_context"].string() && !experience.content().empty() &&
                  item.str("replan_context") == experience.content(),
              "control_replan_context_source_mismatch");
      require(failure_replan && replans > 0 && replans <= 1 &&
                  original->no_progress_decision() == "replan" &&
                  item["error_codes"].array() && item["error_codes"].items().size() == 1 &&
                  item["error_codes"].items()[0].string() &&
                  (item["error_codes"].items()[0].text() == "appworld_auth_401" ||
                   item["error_codes"].items()[0].text() == "appworld_missing_access_token"),
              "control_replan_context_condition_invalid");
      require(experience.content().size() + 128 <= base.max_context_bytes(),
              "control_replan_context_too_large");
    }
    require(
        iterations <= 10000 && no_progress <= 10000 &&
            tighter(iterations, original->max_iterations(), options.num("min_iterations", 2)) &&
            tighter(no_progress, original->max_no_progress(), options.num("min_no_progress", 1)),
        "control_loop_relaxes_limit");
    if (iterations == original->max_iterations() && no_progress == original->max_no_progress() &&
        failure_replan == original->replan_on_failure() && replans == original->max_replans() &&
        (!source_context || experience.content() == original->replan_context()))
      continue;
    // 可执行条件仅使用当前分支的观测事实，不把 reason 当作语义条件。
    // 保守模式仅放行有明确 actor 和收尾约束的候选。GEPA 仍须用完整轨迹预检
    // 必要工作、真实命中和质量收益；运行时继续检查 can_finalize 与未完成分支。
    // 没有这些条件的全局轮数上限仍须显式启用激进模式。
    require(base.aggressive_controls() || iterations == original->max_iterations() ||
                (!item.str("actor_id").empty() && item.flag("require_finalization", false)),
            "control_loop_semantic_scope_required");
    auto *rule = result.add_loop_rules();
    *rule = *original;
    rule->set_rule_id(rule_id(item));
    rule->set_max_iterations(iterations);
    rule->set_max_no_progress(no_progress);
    rule->set_reason(item.str("reason"));
    rule->set_replan_on_failure(failure_replan);
    rule->set_max_replans(replans);
    if (source_context)
      rule->set_replan_context(experience.content());
    if (!replans) {
      rule->set_no_progress_decision("stop");
      rule->clear_replan_context();
    }
    rule->set_actor_id(item.str("actor_id"));
    if (item.has("error_codes")) {
      require(item["error_codes"].array() && item["error_codes"].items().size() <= 8,
              "control_loop_condition_invalid");
      rule->clear_error_codes();
      std::set<std::string> codes;
      for (const auto &code : item["error_codes"].items()) {
        require(code.string() && identifier(code.text()) && code.text().size() <= 128 &&
                    codes.insert(code.text()).second,
                "control_loop_condition_invalid");
        rule->add_error_codes(code.text());
      }
    }
    if (item.has("after_action_rejection")) {
      rule->set_after_action_rejection(item.flag("after_action_rejection", false));
    }
    if (item.has("require_finalization")) {
      rule->set_require_finalization(item.flag("require_finalization", false));
    }
    require(!original->after_action_rejection() || rule->after_action_rejection(),
            "control_loop_relaxes_condition");
    require(!original->require_finalization() || rule->require_finalization(),
            "control_loop_relaxes_condition");
    if (original->error_codes_size()) {
      require(rule->error_codes_size() > 0, "control_loop_relaxes_condition");
      for (const auto &code : rule->error_codes())
        require(std::find(original->error_codes().begin(), original->error_codes().end(), code) !=
                    original->error_codes().end(),
                "control_loop_relaxes_condition");
    }
    rule->set_match_mode(item.str("match_mode", "predicate"));
    require(rule->match_mode().empty() || rule->match_mode() == "predicate",
            "runtime_rule_match_mode_invalid");
    rule->mutable_source_experience()->set_memory_id(experience.memory_id());
    rule->mutable_source_experience()->set_content_revision(experience.content_revision());
    rule->clear_rule_id();
    rule->set_rule_id(rule_id(json(*rule)));
  }
  for (const auto &item : schedules.items()) {
    keys(item, {"memory_id", "before_operation", "after_operation", "require_success", "reason",
                "task_terms", "exclude_terms", "before_argument", "after_argument"});
    require(base.requested_level() >= v1::L2 &&
                std::find(base.host_capabilities().begin(), base.host_capabilities().end(),
                          "schedule.dispatch") != base.host_capabilities().end(),
            "control_schedule_not_authorized");
    const auto memory_id = item.str("memory_id");
    const auto &experience = source(snapshot, base, memory_id);
    auto *rule = result.add_schedule_rules();
    rule->set_target_scope(experience.target_scope());
    rule->set_before_operation(item.str("before_operation"));
    rule->set_after_operation(item.str("after_operation"));
    rule->set_require_success(item.flag("require_success", false));
    rule->set_reason(item.str("reason"));
    rule->set_before_argument(item.str("before_argument"));
    rule->set_after_argument(item.str("after_argument"));
    const auto terms = item.has("task_terms") ? item["task_terms"] : Json(Json::Array{});
    require(terms.array() && terms.items().size() <= 8 &&
                (!terms.items().empty() ||
                 (!rule->before_argument().empty() && !rule->after_argument().empty())),
            "control_schedule_task_guard_required");
    for (const auto &term : terms.items())
      rule->add_task_terms(term.text());
    require(item["exclude_terms"].null() ||
                (item["exclude_terms"].array() && item["exclude_terms"].items().size() <= 8),
            "control_schedule_task_guard_invalid");
    if (item["exclude_terms"].array())
      for (const auto &term : item["exclude_terms"].items())
        rule->add_exclude_terms(term.text());
    rule->mutable_source_experience()->set_memory_id(experience.memory_id());
    rule->mutable_source_experience()->set_content_revision(experience.content_revision());
    rule->set_rule_id(rule_id(json(*rule)));
  }
  validate_schedule_rules(result);
  require(result.action_rules_size() <= 64 && result.loop_rules_size() <= 64,
          "control_rule_capacity_reached");
  return result;
}

// 校验生成规则是否落在宿主声明的执行能力范围，避免发布无法执行的控制。
bool generated_controls_authorized(const v1::RuntimePolicy &candidate,
                                   const v1::RuntimePolicy &current, const Json &options) {
  try {
    Json draft{{"action_rules", Json::Array{}},
               {"loop_rules", Json::Array{}},
               {"schedule_rules", Json::Array{}}};
    for (const auto &rule : candidate.options().action_rules())
      if (rule.has_source_experience()) {
        require(!rule.deny(), "control_deny_not_authorized");
        const auto &entry =
            source(current.snapshot(), current.options(), rule.source_experience().memory_id());
        require(entry.content_revision() == rule.source_experience().content_revision() &&
                    entry.target_scope() == rule.target_scope(),
                "control_source_changed");
        Json arguments = Json::Array{};
        for (const auto &key : rule.required_arguments())
          arguments.push(key);
        Json item{{"memory_id", entry.memory_id()},
                  {"operation", rule.operation()},
                  {"required_arguments", arguments},
                  {"max_failed_attempts", std::size_t(rule.max_failed_attempts())},
                  {"reason", rule.reason()},
                  {"match_mode", rule.match_mode()}};
        if (rule.failure_key_arguments_size()) {
          item["failure_key_arguments"] = Json::Array{};
          for (const auto &key : rule.failure_key_arguments())
            item["failure_key_arguments"].push(key);
        }
        if (rule.terminal_error_codes_size()) {
          item["primitive_id"] = kTerminalPrimitive;
          item["terminal_error_codes"] = Json::Array{};
          item["error_scope_arguments"] = Json::Array{};
          for (const auto &code : rule.terminal_error_codes())
            item["terminal_error_codes"].push(code);
          for (const auto &key : rule.error_scope_arguments())
            item["error_scope_arguments"].push(key);
        }
        if (rule.max_attempts()) {
          item["primitive_id"] = kBudgetPrimitive;
          item["max_attempts"] = std::size_t(rule.max_attempts());
          item["attempt_key_arguments"] = Json::Array{};
          for (const auto &key : rule.attempt_key_arguments())
            item["attempt_key_arguments"].push(key);
        }
        draft["action_rules"].push(std::move(item));
      }
    for (const auto &rule : candidate.options().loop_rules())
      if (rule.has_source_experience()) {
        const auto &entry =
            source(current.snapshot(), current.options(), rule.source_experience().memory_id());
        require(entry.content_revision() == rule.source_experience().content_revision() &&
                    entry.target_scope() == rule.target_scope(),
                "control_source_changed");
        Json item{{"memory_id", entry.memory_id()},
                  {"actor_id", rule.actor_id()},
                  {"max_iterations", std::size_t(rule.max_iterations())},
                  {"max_no_progress", std::size_t(rule.max_no_progress())},
                  {"max_replans", std::size_t(rule.max_replans())},
                  {"replan_on_failure", rule.replan_on_failure()},
                  {"reason", rule.reason()},
                  {"match_mode", rule.match_mode()}};
        if (rule.error_codes_size()) {
          item["error_codes"] = Json::Array{};
          for (const auto &code : rule.error_codes())
            item["error_codes"].push(code);
        }
        if (rule.after_action_rejection())
          item["after_action_rejection"] = true;
        if (rule.require_finalization())
          item["require_finalization"] = true;
        const v1::LoopRule *original = nullptr;
        for (const auto &base_rule : candidate.options().loop_rules())
          if (!base_rule.has_source_experience() &&
              base_rule.target_scope() == entry.target_scope() &&
              (base_rule.actor_id().empty() || base_rule.actor_id() == rule.actor_id()) &&
              (!original || original->actor_id().empty()))
            original = &base_rule;
        if (rule.max_replans() &&
            (!original || rule.replan_context() != original->replan_context()))
          item["replan_context"] = rule.replan_context();
        draft["loop_rules"].push(std::move(item));
      }
    for (const auto &rule : candidate.options().schedule_rules())
      if (rule.has_source_experience()) {
        const auto &entry =
            source(current.snapshot(), current.options(), rule.source_experience().memory_id());
        require(entry.content_revision() == rule.source_experience().content_revision() &&
                    entry.target_scope() == rule.target_scope(),
                "control_source_changed");
        Json task_terms = Json::Array{}, exclude_terms = Json::Array{};
        for (const auto &term : rule.task_terms())
          task_terms.push(term);
        for (const auto &term : rule.exclude_terms())
          exclude_terms.push(term);
        draft["schedule_rules"].push(Json{{"memory_id", entry.memory_id()},
                                          {"before_operation", rule.before_operation()},
                                          {"after_operation", rule.after_operation()},
                                          {"require_success", rule.require_success()},
                                          {"reason", rule.reason()},
                                          {"task_terms", task_terms},
                                          {"exclude_terms", exclude_terms},
                                          {"before_argument", rule.before_argument()},
                                          {"after_argument", rule.after_argument()}});
      }
    if (draft["action_rules"].items().empty() && draft["loop_rules"].items().empty() &&
        draft["schedule_rules"].items().empty())
      return true;
    auto rebuilt = apply_control_draft(
        current.snapshot(), without_generated_controls(candidate.options()), options, draft);
    return json(rebuilt).dump() == json(candidate.options()).dump();
  } catch (...) {
    return false;
  }
}

// 合并不同来源的控制覆盖证据，同一机会不会因重复上报而重复计数。
Json merge_control_coverage(Json coverage, const Json &previous) {
  Json missing = Json::Array{};
  for (auto &pair : coverage["rules"].fields()) {
    auto &counts = pair.second;
    for (const auto *field : {"queries", "triggered", "applied"})
      counts[field] = counts.num(field, 0) + previous["rules"][pair.first].num(field, 0);
    if (!counts.num("applied", 0))
      missing.push(pair.first);
  }
  coverage["unobserved_rules"] = missing;
  coverage["complete"] = missing.items().empty();
  Json unobserved = Json::Array{};
  if (coverage["capabilities"].object())
    for (auto &pair : coverage["capabilities"].fields()) {
      for (const auto *field : {"queries", "triggered", "applied"})
        pair.second[field] =
            pair.second.num(field, 0) + previous["capabilities"][pair.first].num(field, 0);
      if (pair.second.flag("required", false) && !pair.second.num("applied", 0))
        unobserved.push(pair.first);
    }
  coverage["unobserved_capabilities"] = unobserved;
  coverage["level_complete"] = coverage["capabilities"].object() && unobserved.items().empty();
  return coverage;
}

// 统计规则的查询、命中和真实执行情况，区分“生成了规则”与“实际应用了规则”。
Json control_coverage(const Json &artifact, const dataset::v1::TaskEvaluationReport &report) {
  Json result{{"rules", Json::Object{}}, {"capabilities", Json::Object{}}};
  auto items = [](const Json &value) -> const Json::Array & {
    static const Json::Array empty;
    return value.array() ? value.items() : empty;
  };
  std::map<std::string, Json> rules;
  std::set<std::string> capabilities;
  for (const auto &cap : items(artifact["policy"]["options"]["host_capabilities"]))
    capabilities.insert(cap.text());
  const auto level = artifact.str("level", "L0");
  for (const auto *boundary : {"llm", "actions", "schedule", "loop"}) {
    const auto name = std::string(boundary);
    const bool required =
        (name == "llm" && level >= "L1" && capabilities.count("context.request")) ||
        (name == "actions" && level >= "L2" && capabilities.count("action.filter") &&
         !capabilities.count("schedule.dispatch")) ||
        (name == "schedule" && level >= "L2" && capabilities.count("schedule.dispatch")) ||
        (name == "loop" && level >= "L3" && capabilities.count("loop.decide"));
    result["capabilities"][name] =
        Json{{"required", required}, {"queries", 0}, {"triggered", 0}, {"applied", 0}};
  }
  std::string candidate_version;
  const auto configuration = json(report.configuration());
  for (const auto &variant : items(configuration["variants"]))
    if (variant.str("id") == "candidate")
      candidate_version = variant.str("version");
  for (const auto *category : {"action_rules", "loop_rules", "schedule_rules"})
    for (const auto &rule : items(artifact["policy"]["options"][category]))
      if (rule.has("source_experience")) {
        rules.emplace(rule.str("rule_id"), rule);
        result["rules"][rule.str("rule_id")] =
            Json{{"boundary", std::string(category) == "action_rules"     ? "actions"
                              : std::string(category) == "schedule_rules" ? "schedule"
                                                                          : "loop"},
                 {"queries", 0},
                 {"triggered", 0},
                 {"applied", 0}};
      }
  // 改写后的经验文本也必须有真实模型请求回执。对完整改写内容哈希，避免旧文本
  // 的证据错误地批准新文本。
  for (const auto &rewrite : items(artifact["policy"]["options"]["experience_overrides"])) {
    bool disabled = false;
    for (const auto &source : items(artifact["policy"]["options"]["disabled_injections"]))
      disabled |= source.dump() == rewrite["source_experience"].dump();
    if (disabled)
      continue;
    const auto id = "experience-" + rule_id(rewrite);
    Json rule{{"source_experience", rewrite["source_experience"]}};
    for (const auto &experience : items(artifact["policy"]["snapshot"]["experiences"]))
      if (experience.str("memory_id") == rewrite["source_experience"].str("memory_id") &&
          experience.str("content_revision") ==
              rewrite["source_experience"].str("content_revision"))
        rule["target_scope"] = experience["target_scope"];
    rules.emplace(id, std::move(rule));
    result["rules"][id] =
        Json{{"boundary", "llm"}, {"queries", 0}, {"triggered", 0}, {"applied", 0}};
  }
  for (const auto &trial : report.trials()) {
    if (trial.variant_id() != "candidate" || trial.execution_status() != "completed" ||
        (trial.verdict() != "passed" && trial.verdict() != "failed"))
      continue;
    const auto evidence = json(trial.evidence());
    if (candidate_version.empty() || evidence.str("loaded_version") != candidate_version)
      continue;
    const auto traces =
        evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
    for (const auto &trace : traces.items()) {
      std::map<std::string, std::set<std::string>> triggered;
      std::map<std::string, std::string> capability_triggers;
      std::set<std::string> observed, applied;
      auto key = [](const Json &value) {
        return value.str("policy_id") + "\n" + value.str("query_id") + "\n" +
               value.str("decision_id");
      };
      for (const auto &event : items(trace["events"])) {
        if (event["attributes"].str("type") != "policy.query")
          continue;
        const auto &decision = event["attributes"]["decision"];
        const auto &query = decision["query"];
        const auto &advice = query["advice_query"];
        Json identity = advice;
        identity["policy_id"] = decision["policy_id"];
        const auto query_key = key(identity);
        if (!observed.insert(query_key).second)
          continue;
        const auto boundary = query.str("boundary");
        if (result["capabilities"].has(boundary)) {
          auto &counts = result["capabilities"][boundary];
          counts["queries"] = counts.num("queries", 0) + 1;
          bool changed = boundary == "llm" && !decision.str("context").empty() &&
                         !items(decision["experiences"]).empty();
          if (boundary == "actions")
            for (const auto &action : items(decision["actions"]))
              changed |= action.str("decision") == "reject";
          changed |= boundary == "schedule" && decision["schedule"].str("status") == "scheduled";
          changed |= boundary == "loop" && (decision["progress"].str("decision") == "stop" ||
                                            decision["progress"].str("decision") == "replan");
          if (changed) {
            counts["triggered"] = counts.num("triggered", 0) + 1;
            capability_triggers[query_key] = boundary;
          }
        }
        for (const auto &rule : rules) {
          auto &counts = result["rules"][rule.first];
          if (counts.str("boundary") == query.str("boundary") &&
              rule.second.str("target_scope") == advice.str("target_scope") &&
              (query.str("boundary") != "loop" || rule.second.str("actor_id").empty() ||
               rule.second.str("actor_id") == query.str("actor_id", event.str("actor_id")))) {
            counts["queries"] = counts.num("queries", 0) + 1;
            if (query.str("boundary") == "llm")
              for (const auto &source : items(decision["experiences"]))
                if (source.dump() == rule.second["source_experience"].dump())
                  triggered[query_key].insert(rule.first);
          }
        }
        for (const auto &action : items(decision["actions"]))
          if (action.str("decision") == "reject" && rules.count(action.str("rule_id")))
            triggered[query_key].insert(action.str("rule_id"));
        if (decision["schedule"].str("status") == "scheduled")
          for (const auto &id : items(decision["schedule"]["rule_ids"]))
            if (rules.count(id.text()))
              triggered[query_key].insert(id.text());
        const auto &progress = decision["progress"];
        if ((progress.str("decision") == "stop" || progress.str("decision") == "replan") &&
            rules.count(progress.str("rule_id")))
          triggered[query_key].insert(progress.str("rule_id"));
        for (const auto &id : triggered[query_key])
          result["rules"][id]["triggered"] = result["rules"][id].num("triggered", 0) + 1;
      }
      for (const auto &event : items(trace["events"])) {
        if (event.str("kind") != "advice.applied" ||
            event["application"].str("status") != "applied")
          continue;
        const auto query_key = key(event["application"]);
        if (!applied.insert(query_key).second)
          continue;
        const auto &receipt = event["attributes"]["receipt"];
        if (receipt["application"].dump() != event["application"].dump())
          continue;
        const auto capability = capability_triggers.find(query_key);
        if (capability != capability_triggers.end() &&
            receipt.str("boundary") == capability->second) {
          auto &counts = result["capabilities"][capability->second];
          counts["applied"] = counts.num("applied", 0) + 1;
        }
        for (const auto &id : triggered[query_key]) {
          if (result["rules"][id].str("boundary") == "schedule") {
            bool observed_rule = false;
            for (const auto &rule_id : items(receipt["rule_ids"]))
              observed_rule |= rule_id.text() == id;
            if (!observed_rule)
              continue;
          }
          bool source_observed = false;
          for (const auto &source : items(receipt["experiences"]))
            source_observed |= source.dump() == rules[id]["source_experience"].dump();
          if (source_observed)
            result["rules"][id]["applied"] = result["rules"][id].num("applied", 0) + 1;
        }
      }
    }
  }
  return merge_control_coverage(result, Json{});
}
} // 命名空间结束：trajectory::evolution::integration
