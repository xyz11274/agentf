// 运行时只应用当前冻结版本；生成、审批和发布策略属于离线评测路径。
#include "trajectory_evolution/policy_runtime.h"
#include "trajectory_evolution/orchestration.h"
#include "trajectory_evolution/trace.h"
#include "trajectory_evolution/request_binding.h"
#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/experience.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <tuple>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/sha.h>

namespace trajectory::evolution {
namespace {
using agentflow::trajectory::ensure_safe_evolution_data;
constexpr const char* kSchemaOperation = "*";
constexpr const char* kSchemaRequiredNonempty = "$schema_required_nonempty";

void require(bool ok, const char* reason) {
  if(!ok) throw agentflow::trajectory::EvaluationError(reason);
}
template <class Range>
bool contains(const Range& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}
bool identifier(const std::string& value) {
  return !value.empty() && value.size() <= 512
      && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool schedule_matches_task(const v1::ScheduleRule& rule, const std::string& task) {
  if(rule.task_terms().empty()) return false;
  auto lower = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return value;
  };
  const auto text = lower(task);
  for(const auto& term : rule.task_terms()) if(text.find(lower(term)) == std::string::npos) return false;
  for(const auto& term : rule.exclude_terms()) if(text.find(lower(term)) != std::string::npos) return false;
  return true;
}
std::string revision(v1::RuntimePolicy policy) {
  policy.clear_policy_id();
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream stream(&bytes);
    google::protobuf::io::CodedOutputStream coded(&stream);
    coded.SetSerializationDeterministic(true);
    require(policy.SerializeToCodedStream(&coded), "runtime_serialization_failed");
  }
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  require(SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                 digest.data()) != nullptr, "runtime_hash_failed");
  std::string result = "runtime-v2:";
  constexpr char hex[] = "0123456789abcdef";
  for(auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
v1::Level validate_options(const v1::RuntimeOptions& options) {
  validate_request_binding(options);
  validate_schedule_rules(options);
  require(options.requested_level() >= v1::L1 && options.requested_level() <= v1::L3,
          "runtime_level_invalid");
  require(options.fallback() == "strict" || options.fallback() == "downgrade",
          "runtime_fallback_invalid");
  require(options.allowed_users_size() > 0 && options.allowed_users_size() <= 256
              && options.allowed_conversations_size() <= 256
              && options.approved_experiences_size() <= 512
              && options.experience_overrides_size() <= 64
              && options.disabled_injections_size() <= 512
              && options.action_rules_size() <= 64 && options.loop_rules_size() <= 64
              && options.host_capabilities_size() <= 32,
          "runtime_limits_invalid");
  for(const auto& user : options.allowed_users()) require(identifier(user), "runtime_user_invalid");
  for(const auto& conv : options.allowed_conversations()) require(identifier(conv), "runtime_conversation_invalid");
  require(options.max_context_bytes() >= 512 && options.max_context_bytes() <= 16384,
          "runtime_context_limit_invalid");
  const auto& tmpl = options.experience_template();
  auto pos = tmpl.find("{{experience}}");
  require(pos != std::string::npos && tmpl.find("{{experience}}", pos + 14) == std::string::npos
              && tmpl.size() <= options.max_context_bytes(), "runtime_template_invalid");
  std::set<std::string> ids;
  auto approved_source = [&](const v1::ExperienceApproval& source) {
    return std::any_of(options.approved_experiences().begin(), options.approved_experiences().end(), [&](const auto& entry) {
      return entry.memory_id() == source.memory_id() && entry.content_revision() == source.content_revision();
    });
  };
  for(const auto& approval : options.approved_experiences()) {
    require(identifier(approval.memory_id()) && identifier(approval.content_revision())
                && ids.insert(approval.memory_id()).second, "runtime_approval_invalid");
  }
  ids.clear();
  for(const auto& source : options.disabled_injections())
    require(approved_source(source) && ids.insert(source.memory_id()).second,
            "runtime_disabled_injection_invalid");
  ids.clear();
  for(const auto& item : options.experience_overrides()) {
    require(approved_source(item.source_experience())
                && ids.insert(item.source_experience().memory_id()).second
                && !item.content().empty() && item.content().size() <= options.max_context_bytes()
                && item.trigger_pattern().size() <= 2048 && item.observed_operations_size() <= 16,
            "runtime_experience_override_invalid");
    std::set<std::string> operations;
    for(const auto& operation : item.observed_operations())
      require(identifier(operation) && operations.insert(operation).second,
              "runtime_experience_override_invalid");
  }
  ids.clear();
  for(const auto& rule : options.action_rules()) {
    require(rule.match_mode().empty() || rule.match_mode() == "predicate", "runtime_rule_match_mode_invalid");
    const bool schema_rule = rule.operation() == kSchemaOperation
        && rule.required_arguments_size() == 1
        && rule.required_arguments(0) == kSchemaRequiredNonempty;
    const bool repeat_rule = rule.max_failed_attempts() > 0 && rule.required_arguments_size() == 0;
    const bool budget_rule = rule.max_attempts() > 0 && rule.required_arguments_size() == 0;
    require(!rule.has_source_experience() || approved_source(rule.source_experience()), "runtime_rule_source_not_approved");
    require(identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second
                && identifier(rule.target_scope()) && identifier(rule.operation())
                && !rule.reason().empty() && rule.reason().size() <= 1024
                && (rule.deny() || rule.required_arguments_size() > 0 || repeat_rule || budget_rule)
                && rule.required_arguments_size() <= 32
                && rule.max_failed_attempts() <= 16
                && rule.failure_key_arguments_size() <= 4
                && rule.terminal_error_codes_size() <= 8 && rule.error_scope_arguments_size() <= 4
                && (!rule.terminal_error_codes_size() || (repeat_rule && !rule.failure_key_arguments_size()
                    && rule.operation() != kSchemaOperation))
                && (!rule.error_scope_arguments_size() || rule.terminal_error_codes_size())
                && rule.max_attempts() <= 64 && rule.attempt_key_arguments_size() <= 4
                && (!rule.max_attempts() || (budget_rule && !rule.max_failed_attempts()
                    && !rule.terminal_error_codes_size() && !rule.failure_key_arguments_size() && rule.operation() != kSchemaOperation))
                && (!rule.attempt_key_arguments_size() || budget_rule)
                && (!rule.failure_key_arguments_size() || (repeat_rule && rule.operation() != kSchemaOperation))
                && (rule.operation() != kSchemaOperation || ((schema_rule || repeat_rule) && !rule.deny())), "runtime_action_rule_invalid");
    for(const auto& key : rule.required_arguments()) require(identifier(key), "runtime_argument_invalid");
    std::set<std::string> failure_keys;
    for(const auto& key : rule.failure_key_arguments())
      require(identifier(key) && failure_keys.insert(key).second, "runtime_argument_invalid");
    for(const auto& key : rule.error_scope_arguments())
      require(identifier(key) && failure_keys.insert(key).second, "runtime_argument_invalid");
    for(const auto& key : rule.attempt_key_arguments())
      require(identifier(key) && failure_keys.insert(key).second, "runtime_argument_invalid");
    std::set<std::string> codes;
    for(const auto& code : rule.terminal_error_codes())
      require(identifier(code) && code.size() <= 128 && codes.insert(code).second, "runtime_argument_invalid");
  }
  std::set<std::tuple<std::string, std::string, bool>> loop_scopes;
  for(const auto& rule : options.loop_rules()) {
    require(rule.match_mode().empty() || rule.match_mode() == "predicate", "runtime_rule_match_mode_invalid");
    require(!rule.has_source_experience() || approved_source(rule.source_experience()), "runtime_rule_source_not_approved");
    require(identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second
                && identifier(rule.target_scope())
                && (rule.actor_id().empty() || identifier(rule.actor_id()))
                && loop_scopes.emplace(rule.target_scope(), rule.actor_id(), rule.has_source_experience()).second
                && (rule.max_iterations() > 0 || rule.max_no_progress() > 0)
                && rule.max_iterations() <= 10000 && rule.max_no_progress() <= 10000
                && rule.max_replans() <= 16 && !rule.reason().empty() && rule.reason().size() <= 1024
                && rule.error_codes_size() <= 8
                && (rule.no_progress_decision() == "stop" || rule.no_progress_decision() == "replan")
                && (!rule.replan_on_failure() || rule.no_progress_decision() == "replan")
                && (rule.no_progress_decision() != "replan" || (rule.max_replans() > 0
                    && !rule.replan_context().empty()
                    && rule.replan_context().size() + 128 <= options.max_context_bytes())),
            "runtime_loop_rule_invalid");
    std::set<std::string> codes;
    for(const auto& code : rule.error_codes())
      require(identifier(code) && code.size() <= 128 && codes.insert(code).second, "runtime_argument_invalid");
  }
  for(const auto& rule : options.schedule_rules())
    require(ids.insert(rule.rule_id()).second, "runtime_rule_id_duplicate");
  v1::Level level = v1::L0;
  if(options.requested_level() >= v1::L1 && contains(options.host_capabilities(), "context.request")) level = v1::L1;
  if(options.requested_level() >= v1::L2 && contains(options.host_capabilities(), "action.filter")) level = v1::L2;
  if(options.requested_level() >= v1::L2 && contains(options.host_capabilities(), "schedule.dispatch")) level = v1::L2;
  if(options.requested_level() >= v1::L3 && contains(options.host_capabilities(), "loop.decide")) level = v1::L3;
  const auto needed = options.requested_level() == v1::L3 ? "loop.decide"
      : options.requested_level() == v1::L2 ? "action.filter" : "context.request";
  require(options.fallback() != "strict" || contains(options.host_capabilities(), needed)
              || (options.requested_level() == v1::L2 && contains(options.host_capabilities(), "schedule.dispatch")),
          "runtime_host_capability_missing");
  require(options.fallback() != "strict" || level >= options.requested_level(),
          "runtime_host_capability_missing");
  return std::min(level, options.requested_level());
}
bool missing(const google::protobuf::Struct& arguments, const std::string& key) {
  const auto it = arguments.fields().find(key);
  if(it == arguments.fields().end()) return true;
  const auto& value = it->second;
  return value.kind_case() == google::protobuf::Value::kNullValue
      || value.kind_case() == google::protobuf::Value::KIND_NOT_SET
      || (value.kind_case() == google::protobuf::Value::kListValue && value.list_value().values().empty())
      || (value.kind_case() == google::protobuf::Value::kStructValue && value.struct_value().fields().empty())
      || (value.kind_case() == google::protobuf::Value::kStringValue && value.string_value().empty());
}
std::string wrap(const std::string&, const std::string& content) {
  // 版本归因保留在 decision／receipt，不把每个 run 的编号混入模型正文。
  // 无关控制规则变化不能改变相同经验的输入，也不应破坏提示词缓存。
  return "\n<trajectory-experience>\n" + content
      + "\n</trajectory-experience>";
}
bool request_contains(const google::protobuf::Struct& request, const std::string& context) {
  const auto it = request.fields().find("messages");
  if(it == request.fields().end() || it->second.kind_case() != google::protobuf::Value::kListValue) return false;
  unsigned count = 0;
  for(const auto& message : it->second.list_value().values()) {
    if(message.kind_case() != google::protobuf::Value::kStructValue) continue;
    const auto& fields = message.struct_value().fields();
    const auto role = fields.find("role"), content = fields.find("content");
    if(role == fields.end() || role->second.string_value() != "system" || content == fields.end()) continue;
    const auto& text = content->second.string_value();
    for(std::size_t pos = 0; (pos = text.find(context, pos)) != std::string::npos; pos += context.size()) ++count;
  }
  return count == 1;
}
}  // 匿名命名空间

// 编译阶段把经验快照和经校验的选项固定为一个不可变策略版本；后续运行时只读取该版本。
// 这样同一 policy_id 在不同打点处会得到一致的 L1、L2、L3 决策。
v1::RuntimePolicy compile_runtime_policy(v1::ShadowPolicySnapshot snapshot,
                                         const v1::RuntimeOptions& options) {
  require(snapshot.schema_version() == 2, "runtime_experience_contract_missing");
  ShadowRuntime checked(snapshot);
  ensure_safe_evolution_data(options);
  v1::RuntimePolicy policy;
  policy.set_schema_version(1);
  *policy.mutable_snapshot() = std::move(snapshot);
  *policy.mutable_options() = options;
  policy.set_effective_level(validate_options(options));
  const auto& identity = policy.snapshot().identity();
  for(const auto& disabled : options.disabled_injections()) {
    bool matched = false;
    for(const auto& source : policy.snapshot().experiences())
      matched |= source.control_target_type().empty() && source.memory_id() == disabled.memory_id()
          && source.content_revision() == disabled.content_revision();
    require(matched, "runtime_disabled_injection_source_invalid");
  }
  for(const auto& item : options.experience_overrides()) {
    bool matched = false;
    for(const auto& source : policy.snapshot().experiences())
      if(source.control_target_type().empty()
          && source.memory_id() == item.source_experience().memory_id()
          && source.content_revision() == item.source_experience().content_revision()) {
        matched = agentflow::trajectory::experience_fields_valid(
            source.target_scope(), item.trigger_pattern().empty() ? source.trigger_pattern() : item.trigger_pattern(), item.content());
      }
    require(matched, "runtime_experience_override_source_invalid");
  }
  require(contains(options.allowed_users(), identity.user_id())
              && (options.allowed_conversations().empty()
                  || contains(options.allowed_conversations(), identity.conversation_id())),
          "runtime_identity_not_authorized");
  for(const auto& rule : options.action_rules())
    require(contains(policy.snapshot().options().target_scopes(), rule.target_scope()), "runtime_scope_invalid");
  for(const auto& rule : options.loop_rules())
    require(contains(policy.snapshot().options().target_scopes(), rule.target_scope()), "runtime_scope_invalid");
  for(const auto& rule : options.schedule_rules())
    require(contains(policy.snapshot().options().target_scopes(), rule.target_scope()), "runtime_scope_invalid");
  require(policy.ByteSizeLong() <= 1024U * 1024U, "runtime_policy_too_large");
  policy.set_policy_id(revision(policy));
  return policy;
}

PolicyRuntime::PolicyRuntime(v1::RuntimePolicy policy)
    : policy_(std::move(policy)), shadow_(policy_.snapshot()) {
  require(policy_.schema_version() == 1 && policy_.policy_id() == revision(policy_)
              && compile_runtime_policy(policy_.snapshot(), policy_.options()).effective_level()
                   == policy_.effective_level(), "runtime_policy_invalid");
  for(const auto& e : policy_.snapshot().experiences())
    for(const auto& a : policy_.options().approved_experiences())
      if(e.memory_id() == a.memory_id() && e.content_revision() == a.content_revision())
        approved_ids_.insert(e.memory_id());
}

// 决策函数没有副作用：它只把当前打点查询映射为建议、控制或循环结论。
// 业务状态只能在宿主随后提交实际执行结果时推进，不能把策略建议误记为已发生事实。
v1::RuntimeDecision PolicyRuntime::decide(const v1::RuntimeQuery& query) const {
  ensure_safe_evolution_data(query);
  require(query.observed_operations_size() <= 64 && query.error_codes_size() <= 64
      && query.rejected_actions() <= 10000, "runtime_query_invalid");
  for(const auto& operation : query.observed_operations()) require(identifier(operation), "runtime_query_invalid");
  for(const auto& code : query.error_codes())
    require(identifier(code) && code.size() <= 128, "runtime_query_invalid");
  auto eligible = approved_ids_;
  // L1 停用仅改变文字注入；动作、编排与循环仍使用原来源批准。
  if(query.boundary() == "llm") for(const auto& disabled : policy_.options().disabled_injections()) {
    eligible.erase(disabled.memory_id());
    for(const auto& relation : policy_.snapshot().cross_relations())
      if(relation.relation() == "duplicate" && relation.private_memory_id() == disabled.memory_id()
          && relation.private_content_revision() == disabled.content_revision())
        eligible.erase(relation.shared_memory_id());
  }
  std::map<std::string, std::string> triggers;
  for(const auto& rewrite : policy_.options().experience_overrides()) {
    if(query.boundary() != "llm") break; // L1 的适用条件只影响经验注入，不能削弱可执行控制。
    const auto& id = rewrite.source_experience().memory_id();
    if(!rewrite.trigger_pattern().empty()) triggers[id] = rewrite.trigger_pattern();
    if(!rewrite.observed_operations().empty() && std::none_of(
        rewrite.observed_operations().begin(), rewrite.observed_operations().end(),
        [&](const auto& operation) { return contains(query.observed_operations(), operation); })) eligible.erase(id);
    // 当前版本若收窄私有规范记录，已被遮蔽的公共副本不能恢复旧条件或旧正文。
    for(const auto& relation : policy_.snapshot().cross_relations())
      if(relation.relation() == "duplicate" && relation.private_memory_id() == id
          && relation.private_content_revision() == rewrite.source_experience().content_revision())
        eligible.erase(relation.shared_memory_id());
  }
  // 三类边界共用身份、作用域和输入预算校验，保证检索的适用口径一致。
  const auto advice = shadow_.query_advice(query.advice_query(), eligible, triggers);
  require(advice.status() != "error" && identifier(query.execution_id())
              && query.ByteSizeLong() <= 128U * 1024U && query.actions_size() <= 128,
          "runtime_query_invalid");
  require(query.boundary() == "llm" || query.boundary() == "actions" || query.boundary() == "schedule" || query.boundary() == "loop",
          "runtime_boundary_invalid");
  v1::RuntimeDecision result;
  result.set_policy_id(policy_.policy_id());
  *result.mutable_query() = query;
  result.set_status("no_change");
  const auto& options = policy_.options();
  auto source_matches = [&](const auto& rule) {
    if(!rule.has_source_experience()) return true;
    // 可执行规则的来源仍须版本固定且已批准；不能因为无关 L1 建议占满 top-k
    // 就让这条规则的条件消失。
    if(rule.match_mode() == "predicate" || options.aggressive_controls())
      return approved_ids_.count(rule.source_experience().memory_id()) != 0;
    return std::any_of(advice.advice().begin(), advice.advice().end(), [&](const auto& item) {
      return item.experience().memory_id() == rule.source_experience().memory_id()
          && item.experience().content_revision() == rule.source_experience().content_revision();
    });
  };
  // L1 只在模型请求边界生成经验上下文；动作、编排和循环控制均不依赖提示词命中。
  if(query.boundary() == "llm" && policy_.effective_level() >= v1::L1
      && contains(options.host_capabilities(), "context.request")) {
    std::string content;
    std::set<std::string> applied;
    auto append_context = [&](const agentflow::trajectory::evaluation::ExperienceAdvice& item) {
      auto next = content + (content.empty() ? "" : ",\n") + to_json(item, false);
      auto rendered = options.experience_template();
      rendered.replace(rendered.find("{{experience}}"), 14, "[" + next + "]");
      rendered = wrap(policy_.policy_id(), rendered);
      if(rendered.size() > options.max_context_bytes()) return false;
      content = std::move(next);
      result.set_context(std::move(rendered));
      return true;
    };
    if(policy_.effective_level() >= v1::L2 && contains(options.host_capabilities(), "schedule.dispatch")
        && query.observed_operations().empty()) {
      std::size_t hints = 0;
      for(const auto& rule : options.schedule_rules()) {
        if(hints == 2) break;
        if(rule.target_scope() != query.advice_query().target_scope()
            || !schedule_matches_task(rule, query.advice_query().task())
            || (rule.has_source_experience()
                && !approved_ids_.count(rule.source_experience().memory_id()))) continue;
        agentflow::trajectory::evaluation::ExperienceAdvice item;
        item.set_target_scope(rule.target_scope());
        item.set_trigger_pattern("L2 skill scheduling");
        item.set_proposed_change("If this task needs both " + rule.before_operation() + " and "
            + rule.after_operation() + ", propose both in one dispatch batch. Run " + rule.before_operation()
            + " before " + rule.after_operation()
            + (rule.require_success() ? "; run the latter only after the former succeeds."
                                      : "; run the latter after the former finishes.")
            + " Do not add an unnecessary skill merely to match this rule.");
        if(append_context(item)) ++hints;
      }
    }
    std::set<std::pair<std::string, std::string>> injected_content;
    for(const auto& match : advice.advice()) {
      const auto& experience = match.experience();
      // 优先采用已批准的私有重复记录；相关或独立建议仍可补充。若无私有批准记录，
      // 公共副本才可入选。
      if(experience.application_shared() && std::any_of(policy_.snapshot().cross_relations().begin(),
          policy_.snapshot().cross_relations().end(), [&](const auto& r) {
            return r.relation() == "duplicate" && r.shared_memory_id() == experience.memory_id()
                && applied.count(r.private_memory_id());
          })) continue;
      auto approved = std::find_if(options.approved_experiences().begin(), options.approved_experiences().end(),
          [&](const auto& a) { return a.memory_id() == experience.memory_id()
              && a.content_revision() == experience.content_revision(); });
      if(approved == options.approved_experiences().end()) continue;
      if(!agentflow::trajectory::experience_fields_valid(
          experience.target_scope(), experience.trigger_pattern(), experience.content())) continue;
      // 有类型 JSON 保留字段顺序，使 verify() 重算时得到相同的上下文。
      agentflow::trajectory::evaluation::ExperienceAdvice item;
      item.set_target_scope(experience.target_scope());
      item.set_trigger_pattern(experience.trigger_pattern());
      item.set_proposed_change(experience.content());
      for(const auto& rewrite : options.experience_overrides())
        if(rewrite.source_experience().memory_id() == approved->memory_id()
            && rewrite.source_experience().content_revision() == approved->content_revision()) {
          item.set_proposed_change(rewrite.content());
          // 检索与注入使用同一版本的适用条件，不能展示来源记忆的旧条件。
          if(!rewrite.trigger_pattern().empty()) item.set_trigger_pattern(rewrite.trigger_pattern());
        }
      // 同一有效建议可能来自不同私有/公共记录；一次请求只注入一份。
      const auto content_key = std::make_pair(item.target_scope(), item.proposed_change());
      if(injected_content.count(content_key) || !append_context(item)) continue;
      injected_content.insert(content_key);
      *result.add_experiences() = *approved;
      applied.insert(experience.memory_id());
    }
    if(!result.context().empty()) result.set_status("ready");
  } else if(query.boundary() == "actions") {
    // L2 在动作真正派发前逐项给出准入结论；拒绝不等价于一次已执行的业务失败。
    std::set<std::string> action_ids;
    for(const auto& action : query.actions()) {
      require(identifier(action.action_id()) && identifier(action.operation())
                  && action_ids.insert(action.action_id()).second, "runtime_action_id_invalid");
      std::set<std::string> schema_required;
      require(action.schema_required_arguments_size() <= 32, "runtime_action_schema_invalid");
      for(const auto& key : action.schema_required_arguments())
        require(identifier(key) && schema_required.insert(key).second, "runtime_action_schema_invalid");
      auto* decision = result.add_actions();
      decision->set_action_id(action.action_id());
      decision->set_decision("allow");
      if(policy_.effective_level() < v1::L2 || !contains(options.host_capabilities(), "action.filter")) continue;
      for(const auto& rule : options.action_rules()) {
        const bool schema_rule = rule.operation() == kSchemaOperation
            && rule.required_arguments_size() == 1
            && rule.required_arguments(0) == kSchemaRequiredNonempty;
        const bool repeat_rule = rule.max_failed_attempts() > 0 && rule.required_arguments_size() == 0;
        if(rule.target_scope() != query.advice_query().target_scope()
            || (!schema_rule && rule.operation() != kSchemaOperation && rule.operation() != action.operation()) || !source_matches(rule)) continue;
        const auto target = action.target_failures().find(rule.rule_id());
        const auto failures = rule.failure_key_arguments_size() || rule.terminal_error_codes_size() || rule.max_attempts()
            ? (target == action.target_failures().end() ? 0 : target->second) : action.prior_failures();
        bool reject = rule.deny() || (repeat_rule && failures >= rule.max_failed_attempts())
            || (rule.max_attempts() && failures >= rule.max_attempts());
        if(schema_rule) {
          for(const auto& key : action.schema_required_arguments()) reject |= missing(action.arguments(), key);
        } else for(const auto& key : rule.required_arguments()) reject |= missing(action.arguments(), key);
        if(!reject) continue;
        decision->set_decision("reject");
        decision->set_rule_id(rule.rule_id());
        decision->set_reason(rule.reason());
        result.set_status("ready");
        if(rule.has_source_experience()) *result.add_experiences() = rule.source_experience();
        break;
      }
    }
  } else if(query.boundary() == "schedule") {
    // L2 编排仅描述依赖和顺序，实际并发/串行状态仍由宿主的 schedule.observe 打点确认。
    auto effective = options;
    effective.set_requested_level(policy_.effective_level());
    *result.mutable_schedule() = plan_schedule(effective, query);
    result.mutable_schedule()->set_policy_id(policy_.policy_id());
    if(result.schedule().status() == "scheduled") {
      result.set_status("ready");
      for(const auto& id : result.schedule().rule_ids())
        for(const auto& rule : options.schedule_rules()) if(rule.rule_id() == id && rule.has_source_experience())
          *result.add_experiences() = rule.source_experience();
    }
  } else if(query.boundary() == "loop") {
    // L3 根据宿主已经观察到的循环状态作出 continue/replan/stop 建议，不能越过在途动作。
    require(query.iteration() > 0 && query.state_fingerprint().size() <= 128, "runtime_iteration_invalid");
    const auto& previous = query.previous();
    require((previous.execution_id().empty() && previous.iteration() == 0)
                || (previous.execution_id() == query.execution_id() && previous.iteration() <= query.iteration()
                    && previous.policy_id() == policy_.policy_id()
                    && previous.target_scope() == query.advice_query().target_scope()),
            "runtime_loop_state_invalid");
    auto* progress = result.mutable_progress();
    *progress = previous;
    if(query.pending_actions() || policy_.effective_level() < v1::L3
        || !contains(options.host_capabilities(), "loop.decide")) {
      // 宿主仍持有在途动作时，绝不能停止或重规划。
      progress->set_decision("continue");
      return result;
    }
    const v1::LoopRule* rule = nullptr;
    for(const auto& candidate : options.loop_rules())
      if(candidate.target_scope() == query.advice_query().target_scope() && source_matches(candidate)
          && (candidate.actor_id().empty() || candidate.actor_id() == query.actor_id())
          && (!candidate.require_finalization() || query.can_finalize())
          && (!candidate.after_action_rejection() || query.rejected_actions())
          && (candidate.error_codes().empty() || std::any_of(candidate.error_codes().begin(), candidate.error_codes().end(),
              [&](const auto& code) { return contains(query.error_codes(), code); }))
          && (!rule || (rule->actor_id().empty() && !candidate.actor_id().empty())
              || (rule->actor_id() == candidate.actor_id() && candidate.has_source_experience()))) rule = &candidate;
    // 同一轮次只能重放同一状态指纹，保证重试幂等且不会吞掉并发分支的新结果。
    if(previous.iteration() == query.iteration()) {
      require(previous.fingerprint() == query.state_fingerprint(), "runtime_duplicate_iteration_changed");
    } else {
      progress->set_execution_id(query.execution_id());
      progress->set_policy_id(policy_.policy_id());
      progress->set_target_scope(query.advice_query().target_scope());
      progress->set_iteration(query.iteration());
      progress->set_fingerprint(query.state_fingerprint());
      if(previous.decision() != "stop") {
        progress->set_decision("continue");
        progress->clear_rule_id();
        progress->clear_reason();
        progress->set_no_progress(std::max({query.repeated_action_steps(),
            options.aggressive_controls() ? query.repeated_failure_steps() : 0U,
            !query.state_fingerprint().empty() && previous.iteration() > 0
                && previous.fingerprint() == query.state_fingerprint() ? previous.no_progress() + 1 : 0U}));
        if(rule) {
          if(rule->max_iterations() && query.iteration() > rule->max_iterations()) {
            progress->set_decision("stop");
          } else if(rule->max_no_progress() && progress->no_progress() >= rule->max_no_progress()) {
            if(rule->no_progress_decision() == "replan" && progress->replans() < rule->max_replans()) {
              progress->set_decision("replan");
              progress->set_replans(progress->replans() + 1);
              progress->set_no_progress(0);
            } else progress->set_decision("stop");
          } else if(rule->replan_on_failure()
              && (rule->has_source_experience() ? query.repeated_failure_steps() : query.failed_actions())
              && progress->replans() < rule->max_replans()) {
            // 学习得到的恢复规则必须看到重复的结构化失败；单次动作失败不足以证明
            // 再开一轮循环会有收益。人工基线规则仍保留其显式失败动作语义。
            progress->set_decision("replan");
            progress->set_replans(progress->replans() + 1);
          }
          if(progress->decision() != "continue") {
            progress->set_rule_id(rule->rule_id());
            progress->set_reason(rule->reason());
          }
        }
      }
    }
    if(progress->decision() != "continue") {
      result.set_status("ready");
      if(rule && rule->has_source_experience()) *result.add_experiences() = rule->source_experience();
      if(progress->decision() == "replan") {
        require(rule != nullptr, "runtime_replan_rule_missing");
        result.set_context(wrap(policy_.policy_id(), rule->replan_context()));
        require(result.context().size() <= options.max_context_bytes(), "runtime_replan_context_too_large");
      }
    }
  }
  return result;
}

v1::RuntimeReceipt PolicyRuntime::verify(const v1::RuntimeQuery& query,
                                        const v1::RuntimeObservation& observation) const {
  return verify(query, observation, policy_.options());
}

v1::RuntimeReceipt PolicyRuntime::verify(const v1::RuntimeQuery& query,
                                        const v1::RuntimeObservation& observation,
                                        const v1::RuntimeOptions& request_binding) const {
  validate_request_binding(request_binding);
  require(observation.ByteSizeLong() <= 1024U * 1024U, "runtime_observation_too_large");
  ensure_safe_evolution_data(observation);
  const auto decision = decide(query);
  v1::RuntimeReceipt receipt;
  *receipt.mutable_identity() = query.advice_query().identity();
  receipt.set_execution_id(query.execution_id());
  receipt.set_boundary(query.boundary());
  receipt.set_call_id(observation.call_id());
  *receipt.mutable_experiences() = decision.experiences();
  auto* application = receipt.mutable_application();
  application->set_query_id(query.advice_query().query_id());
  application->set_decision_id(query.advice_query().decision_id());
  application->set_policy_id(policy_.policy_id());
  application->set_policy_revision(policy_.policy_id());
  bool applied = false;
  if(decision.status() == "ready") {
    if(!decision.context().empty()) {
      applied = observation.transport_allowed() && identifier(observation.call_id())
          && (request_binding.request_context_pointer().empty()
              ? request_contains(observation.request(), decision.context())
              : bound_request_contains(observation.request(), decision.context(), request_binding));
    } else if(query.boundary() == "actions") {
      applied = true;
      for(const auto& action : decision.actions()) {
        if(action.decision() != "reject") continue;
        receipt.add_action_ids(action.action_id());
        applied &= contains(observation.skipped_action_ids(), action.action_id())
            && !contains(observation.executed_action_ids(), action.action_id());
      }
    } else if(query.boundary() == "schedule") {
      try {
        auto state = begin_schedule(decision.schedule());
        require(observation.schedule_events_size() <= 512, "schedule_observation_limit");
        for(const auto& event : observation.schedule_events())
          state = advance_schedule(std::move(state), event.action_id(), event.status());
        applied = schedule_finished(state);
        for(const auto& action : state.actions()) {
          if(action.status() == "skipped") applied &= contains(observation.skipped_action_ids(), action.action_id())
              && !contains(observation.executed_action_ids(), action.action_id());
          else applied &= contains(observation.executed_action_ids(), action.action_id());
        }
        if(applied) for(const auto& id : decision.schedule().rule_ids()) receipt.add_rule_ids(id);
      } catch(const agentflow::trajectory::EvaluationError&) { applied = false; }
    } else if(query.boundary() == "loop") {
      applied = decision.progress().decision() == "stop" && observation.loop_decision() == "stop"
          && !query.pending_actions();
    }
  }
  application->set_status(applied ? "applied" : (decision.status() == "ready" ? "failed" : "rejected"));
  application->set_reason(applied ? "host_boundary_verified" : "application_not_observed");
  return receipt;
}

}  // trajectory::evolution 命名空间
