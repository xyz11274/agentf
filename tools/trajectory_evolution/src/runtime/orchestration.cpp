// 编排控制：根据规则与依赖关系规划动作或子任务的执行顺序。
// 先检查可用能力和动作身份，再决定并行、串行或跳过；不能凭文字名称猜测能力。
// 独立分支继续执行，依赖失败的分支保留明确原因和可回放状态。

#include "trajectory_evolution/orchestration.h"
#include "agentflow/trajectory/hybrid_evaluator.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace trajectory::evolution {
namespace {
// 统一检查前置条件；失败时抛出固定错误码，不把原始输入拼入诊断。
void require(bool value, const char *error) {
  if (!value)
    throw agentflow::trajectory::EvaluationError(error);
}

// 检查标识符是否符合协议允许的字符和长度，拒绝把任意路径当作身份使用。
bool identifier(const std::string &value) {
  return !value.empty() && value.size() <= 512 &&
         std::none_of(value.begin(), value.end(),
                      [](unsigned char c) { return c < 32 || c == 127; });
}

// 判断状态或事件是否已经终结，供收尾与重复回执处理使用。
bool terminal(const std::string &value) {
  return value == "succeeded" || value == "failed" || value == "cancelled" || value == "unknown" ||
         value == "skipped";
}

// 只规范化 ASCII 字母大小写，保持中文和其他 UTF-8 字节不变。
std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  });
  return value;
}

// 检查当前请求是否满足规则的任务匹配条件，匹配结果不代表业务已成功。
bool matches_task(const v1::ScheduleRule &rule, const std::string &task) {
  if (rule.task_terms().empty() && rule.exclude_terms().empty())
    return true;
  const auto text = lower_ascii(task);
  for (const auto &term : rule.task_terms())
    if (text.find(lower_ascii(term)) == std::string::npos)
      return false;
  for (const auto &term : rule.exclude_terms())
    if (text.find(lower_ascii(term)) != std::string::npos)
      return false;
  return true;
}

// 判断候选约束是否相互排斥，避免同时发布不可同时满足的安排。
bool mutually_exclusive(const v1::ScheduleRule &left, const v1::ScheduleRule &right) {
  auto excludes_required = [](const v1::ScheduleRule &required, const v1::ScheduleRule &excluded) {
    for (const auto &term : required.task_terms())
      for (const auto &forbidden : excluded.exclude_terms())
        if (lower_ascii(term).find(lower_ascii(forbidden)) != std::string::npos)
          return true;
    return false;
  };
  return excludes_required(left, right) || excludes_required(right, left);
}

// 编排依赖必须无环，否则没有可执行起点或无法收敛；不能靠超时掩盖非法计划。
bool acyclic(const v1::SchedulePlan &plan) {
  std::set<std::string> done;
  bool advanced = true;
  while (advanced) {
    advanced = false;
    for (const auto &action : plan.actions())
      if (!done.count(action.action_id())) {
        bool ready = true;
        for (const auto &dependency : action.after())
          ready &= done.count(dependency.action_id()) != 0;
        if (ready) {
          done.insert(action.action_id());
          advanced = true;
        }
      }
  }
  return done.size() == std::size_t(plan.actions_size());
}

// 验证编排计划的目标、依赖和边界，拒绝循环依赖或不可执行的动作。
void validate_plan(const v1::SchedulePlan &plan) {
  require(identifier(plan.execution_id()) && plan.actions_size() > 0 && plan.actions_size() <= 128,
          "schedule_plan_invalid");
  std::set<std::string> ids;
  for (const auto &action : plan.actions())
    require(identifier(action.action_id()) && ids.insert(action.action_id()).second,
            "schedule_action_invalid");
  for (const auto &action : plan.actions()) {
    std::set<std::string> predecessors;
    require(action.after_size() <= 127, "schedule_dependency_limit");
    for (const auto &dependency : action.after())
      require(dependency.action_id() != action.action_id() && ids.count(dependency.action_id()) &&
                  predecessors.insert(dependency.action_id()).second,
              "schedule_dependency_invalid");
  }
  require(acyclic(plan), "schedule_dependency_cycle");
}

// 整理当前编排任务状态，后续可提交项由依赖条件决定。
std::map<std::string, std::string> states(const v1::ScheduleState &state) {
  validate_plan(state.plan());
  require(state.actions_size() == state.plan().actions_size(), "schedule_state_invalid");
  std::map<std::string, std::string> result;
  for (const auto &action : state.actions()) {
    require(action.status() == "waiting" || action.status() == "running" ||
                terminal(action.status()),
            "schedule_status_invalid");
    require(result.emplace(action.action_id(), action.status()).second, "schedule_state_invalid");
  }
  for (const auto &action : state.plan().actions())
    require(result.count(action.action_id()), "schedule_state_invalid");
  return result;
}
}

// 检查编排规则是否与运行时动作契约一致，避免生成后永远无法命中。
void validate_schedule_rules(const v1::RuntimeOptions &options) {
  require(options.schedule_rules_size() <= 64, "schedule_rule_limit");
  std::set<std::string> ids, guards;
  for (const auto &rule : options.schedule_rules()) {
    require(identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second &&
                identifier(rule.target_scope()) && identifier(rule.before_operation()) &&
                identifier(rule.after_operation()) && !rule.reason().empty() &&
                rule.reason().size() <= 1024,
            "schedule_rule_invalid");
    require(rule.task_terms_size() <= 8 && rule.exclude_terms_size() <= 8,
            "schedule_task_guard_invalid");
    require((rule.before_argument().empty() && rule.after_argument().empty()) ||
                (identifier(rule.before_argument()) && identifier(rule.after_argument())),
            "schedule_resource_guard_invalid");
    std::set<std::string> terms;
    for (const auto *list : {&rule.task_terms(), &rule.exclude_terms()})
      for (const auto &term : *list)
        require(term.size() >= 2 && term.size() <= 128 && identifier(term) &&
                    terms.insert(lower_ascii(term)).second,
                "schedule_task_guard_invalid");
    std::vector<std::string> ordered_terms, ordered_exclusions;
    for (const auto &term : rule.task_terms())
      ordered_terms.push_back(lower_ascii(term));
    for (const auto &term : rule.exclude_terms())
      ordered_exclusions.push_back(lower_ascii(term));
    std::sort(ordered_terms.begin(), ordered_terms.end());
    std::sort(ordered_exclusions.begin(), ordered_exclusions.end());
    std::string guard =
        rule.target_scope() + "\n" + rule.before_operation() + "\n" + rule.after_operation();
    guard += "\nresource:" + rule.before_argument() + "\n" + rule.after_argument();
    for (const auto &term : ordered_terms)
      guard += "\n+" + term;
    for (const auto &term : ordered_exclusions)
      guard += "\n-" + term;
    require(guards.insert(guard).second, "schedule_rule_duplicate");
    require(!mutually_exclusive(rule, rule), "schedule_task_guard_invalid");
    if (rule.has_source_experience()) {
      bool approved = false;
      for (const auto &source : options.approved_experiences())
        approved |= source.memory_id() == rule.source_experience().memory_id() &&
                    source.content_revision() == rule.source_experience().content_revision();
      require(approved, "runtime_rule_source_not_approved");
    }
  }
  // 拒绝任何能与其中至少一条规则共同成立的依赖环。
  // 明确相互排斥的守卫可以表示不同任务工作流。
  for (const auto &focal : options.schedule_rules()) {
    v1::SchedulePlan plan;
    for (const auto &rule : options.schedule_rules()) {
      if (rule.target_scope() != focal.target_scope() || mutually_exclusive(focal, rule) ||
          rule.before_operation() == rule.after_operation())
        continue;
      for (const auto &name : {rule.before_operation(), rule.after_operation()}) {
        bool found = false;
        for (const auto &action : plan.actions())
          found |= action.action_id() == name;
        if (!found)
          plan.add_actions()->set_action_id(name);
      }
      for (auto &action : *plan.mutable_actions())
        if (action.action_id() == rule.after_operation())
          action.add_after()->set_action_id(rule.before_operation());
    }
    require(acyclic(plan), "schedule_rule_cycle");
  }
}

// 结合可用技能和依赖生成串并行安排；不把模型建议直接当作已提交任务。
v1::SchedulePlan plan_schedule(const v1::RuntimeOptions &options, const v1::RuntimeQuery &query) {
  validate_schedule_rules(options);
  v1::SchedulePlan original;
  original.set_execution_id(query.execution_id());
  original.set_status("unchanged");
  for (const auto &action : query.actions()) {
    require(identifier(action.operation()), "schedule_operation_invalid");
    auto *item = original.add_actions();
    item->set_action_id(action.action_id());
    for (const auto &dependency : action.after()) {
      require(dependency.rule_id().empty(), "schedule_host_rule_id_invalid");
      *item->add_after() = dependency;
    }
  }
  validate_plan(original);
  if (options.requested_level() < v1::L2 ||
      std::find(options.host_capabilities().begin(), options.host_capabilities().end(),
                "schedule.dispatch") == options.host_capabilities().end())
    return original;
  auto result = original;
  for (const auto &rule : options.schedule_rules()) {
    if (rule.target_scope() != query.advice_query().target_scope() ||
        !matches_task(rule, query.advice_query().task()))
      continue;
    std::size_t before_count = 0, after_count = 0;
    for (const auto &action : query.actions()) {
      before_count += action.operation() == rule.before_operation();
      after_count += action.operation() == rule.after_operation();
    }
    const bool ambiguous = rule.has_source_experience() &&
                           rule.before_operation() != rule.after_operation() &&
                           (before_count > 1 || after_count > 1);
    bool changed = false;
    for (int i = 0; i < query.actions_size(); ++i)
      if (query.actions(i).operation() == rule.after_operation()) {
        auto *target = result.mutable_actions(i);
        for (int j = 0; j < query.actions_size(); ++j) {
          const auto &before = query.actions(j);
          if (before.operation() != rule.before_operation() ||
              (rule.before_operation() == rule.after_operation() && j >= i))
            continue;
          // 只用显式资源身份配对；不同产物与缺失参数不推断为同一工作流。
          const bool resource_bound = !rule.before_argument().empty();
          if (resource_bound) {
            const auto &outputs = before.arguments().fields();
            const auto &inputs = query.actions(i).arguments().fields();
            const auto output = outputs.find(rule.before_argument()),
                       input = inputs.find(rule.after_argument());
            if (output == outputs.end() || input == inputs.end() ||
                output->second.kind_case() != google::protobuf::Value::kStringValue ||
                input->second.kind_case() != google::protobuf::Value::kStringValue ||
                output->second.string_value().empty() ||
                output->second.string_value().size() > 512 ||
                output->second.string_value().find("<redacted") != std::string::npos ||
                output->second.string_value().find("[REDACTED") != std::string::npos ||
                output->second.string_value().find("[redacted") != std::string::npos ||
                output->second.string_value() != input->second.string_value())
              continue;
          }
          // 操作级学习无法判断多个独立执行实例中哪些应当关联。
          // 只能细化已知依赖，不猜测全连接关系，
          // 避免一次失败取消本不相关的工作。
          if (ambiguous && !resource_bound &&
              std::none_of(
                  original.actions(i).after().begin(), original.actions(i).after().end(),
                  [&](const auto &edge) { return edge.action_id() == before.action_id(); }))
            continue;
          v1::ActionDependency *edge = nullptr;
          for (auto &existing : *target->mutable_after())
            if (existing.action_id() == before.action_id())
              edge = &existing;
          if (!edge) {
            edge = target->add_after();
            edge->set_action_id(before.action_id());
          } else if (!rule.require_success() || edge->require_success())
            continue;
          edge->set_require_success(edge->require_success() || rule.require_success());
          edge->set_rule_id(rule.rule_id());
          changed = true;
        }
      }
    if (changed)
      result.add_rule_ids(rule.rule_id());
  }
  if (!acyclic(result)) {
    // 学习到的顺序可能与本批次已有依赖冲突。
    // 此时保留完整原计划，不能悄悄删除宿主依赖。
    original.set_status("conflict");
    return original;
  }
  if (result.rule_ids_size())
    result.set_status("scheduled");
  validate_plan(result);
  return result;
}

// 初始化编排执行状态，固定本批次的目标身份与依赖关系。
v1::ScheduleState begin_schedule(const v1::SchedulePlan &plan) {
  validate_plan(plan);
  v1::ScheduleState state;
  *state.mutable_plan() = plan;
  for (const auto &action : plan.actions()) {
    auto *item = state.add_actions();
    item->set_action_id(action.action_id());
    item->set_status("waiting");
  }
  return state;
}

// 计算此刻真正可执行的动作集合，已终止或依赖未完成的动作不重复派发。
std::vector<std::string> ready_schedule(const v1::ScheduleState &state) {
  const auto observed = states(state);
  std::vector<std::string> ready;
  for (const auto &action : state.plan().actions())
    if (observed.at(action.action_id()) == "waiting") {
      bool allowed = true;
      for (const auto &dependency : action.after()) {
        const auto &status = observed.at(dependency.action_id());
        allowed &= dependency.require_success() ? status == "succeeded" : terminal(status);
      }
      if (allowed)
        ready.push_back(action.action_id());
    }
  return ready;
}

// 只更新本动作与受其依赖影响的后续节点，独立分支保留自己的状态。
v1::ScheduleState advance_schedule(v1::ScheduleState state, const std::string &action_id,
                                   const std::string &status) {
  const auto before = states(state);
  const auto found = before.find(action_id);
  require(found != before.end() &&
              (status == "running" || (terminal(status) && status != "skipped")),
          "schedule_transition_invalid");
  if (found->second == status)
    return state;
  if (status == "running") {
    const auto ready = ready_schedule(state);
    require(std::find(ready.begin(), ready.end(), action_id) != ready.end(),
            "schedule_predecessor_pending");
  } else
    require(found->second == "running" || (found->second == "waiting" && status == "cancelled"),
            "schedule_action_not_running");
  for (auto &item : *state.mutable_actions())
    if (item.action_id() == action_id)
      item.set_status(status);
  // 前置任务失败只影响明确要求其成功的后代。
  bool changed = true;
  while (changed) {
    changed = false;
    const auto current = states(state);
    for (int i = 0; i < state.actions_size(); ++i) {
      auto *item = state.mutable_actions(i);
      if (item->status() != "waiting")
        continue;
      const v1::ScheduledAction *action = nullptr;
      for (const auto &candidate : state.plan().actions())
        if (candidate.action_id() == item->action_id())
          action = &candidate;
      for (const auto &dependency : action->after())
        if (dependency.require_success() && terminal(current.at(dependency.action_id())) &&
            current.at(dependency.action_id()) != "succeeded") {
          item->set_status("skipped");
          changed = true;
          break;
        }
    }
  }
  return state;
}

// 完成表示计划中的动作已进入终态，不等于所有业务目标成功。
bool schedule_finished(const v1::ScheduleState &state) {
  const auto observed = states(state);
  return std::all_of(observed.begin(), observed.end(),
                     [](const auto &item) { return terminal(item.second); });
}

} // 命名空间结束：trajectory::evolution
