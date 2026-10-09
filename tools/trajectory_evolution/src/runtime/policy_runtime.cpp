// 策略运行时：解释固定版本的 L1 经验、L2 编排和 L3 循环控制。
// 读取策略不会触发在线模型反思；决策使用当前请求、结构条件和执行事实。
// 每次应用都返回归属信息，便于区分规则命中、建议被采纳与真实执行效果。

// 运行时只应用当前冻结版本；生成、审批和发布策略属于离线评测路径。
#include "trajectory_evolution/policy_runtime.h"
#include "trajectory_evolution/orchestration.h"
#include "trajectory_evolution/trace.h"
#include "trajectory_evolution/request_binding.h"
#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/experience.h"
#include "../service/integration_json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <tuple>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/util/message_differencer.h>
#include <openssl/sha.h>

namespace trajectory::evolution {
namespace {
using agentflow::trajectory::ensure_safe_evolution_data;
constexpr const char *kSchemaOperation = "*";
constexpr const char *kSchemaRequiredNonempty = "$schema_required_nonempty";

// 统一检查前置条件；失败时抛出固定错误码，不把原始输入拼入诊断。
void require(bool ok, const char *reason) {
  if (!ok)
    throw agentflow::trajectory::EvaluationError(reason);
}

template <class Range> bool contains(const Range &values, const std::string &value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

// 检查标识符是否符合协议允许的字符和长度，拒绝把任意路径当作身份使用。
bool identifier(const std::string &value) {
  return !value.empty() && value.size() <= 512 &&
         std::none_of(value.begin(), value.end(),
                      [](unsigned char c) { return c < 32 || c == 127; });
}

// 提取“<identifier> 执行者 / executor”形式的显式运行角色约束。
// 标识符内容不绑定具体框架；运行时只与打点携带的 actor_id 做确定性比较。
std::string explicit_actor_anchor(const std::string &condition) {
  auto lowered = condition;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return c < 0x80 ? std::tolower(c) : c; });
  const std::array<std::string, 5> markers{"执行者", "执行器", " executor", " actor",
                                            " agent"};
  for (const auto &marker : markers) {
    const auto at = lowered.find(marker);
    if (at == std::string::npos)
      continue;
    auto end = at;
    while (end && std::isspace(static_cast<unsigned char>(lowered[end - 1])))
      --end;
    auto begin = end;
    while (begin) {
      const auto c = static_cast<unsigned char>(lowered[begin - 1]);
      if (!(std::isalnum(c) || c == '_' || c == '-' || c == ':' || c == '/' || c == '.'))
        break;
      --begin;
    }
    if (begin != end)
      return lowered.substr(begin, end - begin);
  }
  return {};
}

bool actor_condition_matches(const std::string &condition, const std::string &actor_id) {
  const auto anchor = explicit_actor_anchor(condition);
  if (anchor.empty() || actor_id.empty())
    return true;
  auto actor = actor_id;
  std::transform(actor.begin(), actor.end(), actor.begin(),
                 [](unsigned char c) { return c < 0x80 ? std::tolower(c) : c; });
  return actor == anchor;
}

// 读取策略或来源修订，避免使用与当前内容不匹配的旧证据。
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
  require(SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
                 digest.data()) != nullptr,
          "runtime_hash_failed");
  std::string result = "runtime-v2:";
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : digest) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

// 检查运行配置的枚举、范围和相互依赖，不把非法参数静默当成默认值。
v1::Level validate_options(const v1::RuntimeOptions &options) {
  validate_request_binding(options);
  validate_schedule_rules(options);
  require(options.requested_level() >= v1::L1 && options.requested_level() <= v1::L3,
          "runtime_level_invalid");
  require(options.fallback() == "strict" || options.fallback() == "downgrade",
          "runtime_fallback_invalid");
  require(options.allowed_users_size() > 0 && options.allowed_users_size() <= 256 &&
              options.allowed_conversations_size() <= 256 &&
              options.approved_experiences_size() <= 512 &&
              options.experience_overrides_size() <= 64 &&
              options.l1_prevention_sources_size() <= 16 &&
              options.disabled_injections_size() <= 512 && options.action_rules_size() <= 64 &&
              options.loop_rules_size() <= 64 && options.host_capabilities_size() <= 32,
          "runtime_limits_invalid");
  for (const auto &user : options.allowed_users())
    require(identifier(user), "runtime_user_invalid");
  for (const auto &conv : options.allowed_conversations())
    require(identifier(conv), "runtime_conversation_invalid");
  require(options.max_context_bytes() >= 512 && options.max_context_bytes() <= 16384,
          "runtime_context_limit_invalid");
  const auto &tmpl = options.experience_template();
  auto pos = tmpl.find("{{experience}}");
  require(pos != std::string::npos && tmpl.find("{{experience}}", pos + 14) == std::string::npos &&
              tmpl.size() <= options.max_context_bytes(),
          "runtime_template_invalid");
  std::set<std::string> ids;
  auto approved_source = [&](const v1::ExperienceApproval &source) {
    return std::any_of(options.approved_experiences().begin(), options.approved_experiences().end(),
                       [&](const auto &entry) {
                         return entry.memory_id() == source.memory_id() &&
                                entry.content_revision() == source.content_revision();
                       });
  };
  for (const auto &approval : options.approved_experiences()) {
    require(identifier(approval.memory_id()) && identifier(approval.content_revision()) &&
                ids.insert(approval.memory_id()).second,
            "runtime_approval_invalid");
  }
  ids.clear();
  for (const auto &source : options.disabled_injections())
    require(approved_source(source) && ids.insert(source.memory_id()).second,
            "runtime_disabled_injection_invalid");
  ids.clear();
  for (const auto &item : options.experience_overrides()) {
    require(approved_source(item.source_experience()) &&
                ids.insert(item.source_experience().memory_id()).second &&
                !item.content().empty() && item.content().size() <= options.max_context_bytes() &&
                item.trigger_pattern().size() <= 2048 && item.observed_operations_size() <= 16,
            "runtime_experience_override_invalid");
    std::set<std::string> operations;
    for (const auto &operation : item.observed_operations())
      require(identifier(operation) && operations.insert(operation).second,
              "runtime_experience_override_invalid");
  }
  ids.clear();
  for (const auto &memory : options.l1_prevention_sources()) {
    const bool active_override = std::any_of(
        options.experience_overrides().begin(), options.experience_overrides().end(),
        [&](const auto &item) {
          return item.source_experience().memory_id() == memory &&
                 approved_source(item.source_experience());
        });
    const bool disabled = std::any_of(
        options.disabled_injections().begin(), options.disabled_injections().end(),
        [&](const auto &source) { return source.memory_id() == memory; });
    require(identifier(memory) && ids.insert(memory).second && active_override && !disabled,
            "runtime_prevention_source_invalid");
  }
  ids.clear();
  for (const auto &rule : options.action_rules()) {
    require(rule.match_mode().empty() || rule.match_mode() == "predicate",
            "runtime_rule_match_mode_invalid");
    const bool schema_rule = rule.operation() == kSchemaOperation &&
                             rule.required_arguments_size() == 1 &&
                             rule.required_arguments(0) == kSchemaRequiredNonempty;
    const bool repeat_rule = rule.max_failed_attempts() > 0 && rule.required_arguments_size() == 0;
    const bool budget_rule = rule.max_attempts() > 0 && rule.required_arguments_size() == 0;
    require(!rule.has_source_experience() || approved_source(rule.source_experience()),
            "runtime_rule_source_not_approved");
    require(identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second &&
                identifier(rule.target_scope()) && identifier(rule.operation()) &&
                !rule.reason().empty() && rule.reason().size() <= 1024 &&
                (rule.deny() || rule.required_arguments_size() > 0 || repeat_rule || budget_rule) &&
                rule.required_arguments_size() <= 32 && rule.max_failed_attempts() <= 16 &&
                rule.failure_key_arguments_size() <= 4 && rule.terminal_error_codes_size() <= 8 &&
                rule.error_scope_arguments_size() <= 4 &&
                (!rule.terminal_error_codes_size() ||
                 (repeat_rule && !rule.failure_key_arguments_size() &&
                  rule.operation() != kSchemaOperation)) &&
                (!rule.error_scope_arguments_size() || rule.terminal_error_codes_size()) &&
                rule.max_attempts() <= 64 && rule.attempt_key_arguments_size() <= 4 &&
                (!rule.max_attempts() ||
                 (budget_rule && !rule.max_failed_attempts() && !rule.terminal_error_codes_size() &&
                  !rule.failure_key_arguments_size() && rule.operation() != kSchemaOperation)) &&
                (!rule.attempt_key_arguments_size() || budget_rule) &&
                (!rule.failure_key_arguments_size() ||
                 (repeat_rule && rule.operation() != kSchemaOperation)) &&
                (rule.operation() != kSchemaOperation ||
                 ((schema_rule || repeat_rule) && !rule.deny())),
            "runtime_action_rule_invalid");
    for (const auto &key : rule.required_arguments())
      require(identifier(key), "runtime_argument_invalid");
    std::set<std::string> failure_keys;
    for (const auto &key : rule.failure_key_arguments())
      require(identifier(key) && failure_keys.insert(key).second, "runtime_argument_invalid");
    for (const auto &key : rule.error_scope_arguments())
      require(identifier(key) && failure_keys.insert(key).second, "runtime_argument_invalid");
    for (const auto &key : rule.attempt_key_arguments())
      require(identifier(key) && failure_keys.insert(key).second, "runtime_argument_invalid");
    std::set<std::string> codes;
    for (const auto &code : rule.terminal_error_codes())
      require(identifier(code) && code.size() <= 128 && codes.insert(code).second,
              "runtime_argument_invalid");
  }
  std::map<std::tuple<std::string, std::string, bool>, std::vector<const v1::LoopRule *>> loop_scopes;
  for (const auto &rule : options.loop_rules()) {
    require(rule.match_mode().empty() || rule.match_mode() == "predicate",
            "runtime_rule_match_mode_invalid");
    require(!rule.has_source_experience() || approved_source(rule.source_experience()),
            "runtime_rule_source_not_approved");
    require(
        identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second &&
            identifier(rule.target_scope()) &&
            (rule.actor_id().empty() || identifier(rule.actor_id())) &&
            (rule.max_iterations() > 0 || rule.max_no_progress() > 0) &&
            rule.max_iterations() <= 10000 && rule.max_no_progress() <= 10000 &&
            rule.max_replans() <= 16 && !rule.reason().empty() && rule.reason().size() <= 1024 &&
            rule.error_codes_size() <= 8 &&
            (rule.no_progress_decision() == "stop" || rule.no_progress_decision() == "replan") &&
            (!rule.replan_on_failure() || rule.no_progress_decision() == "replan") &&
            (rule.no_progress_decision() != "replan" ||
             (rule.max_replans() > 0 && !rule.replan_context().empty() &&
              rule.replan_context().size() + 128 <= options.max_context_bytes())),
        "runtime_loop_rule_invalid");
    auto &same_scope = loop_scopes[{rule.target_scope(), rule.actor_id(), rule.has_source_experience()}];
    for (const auto *previous : same_scope)
      require(disjoint_failure_recovery_rules(*previous, rule), "runtime_loop_rule_invalid");
    same_scope.push_back(&rule);
    std::set<std::string> codes;
    for (const auto &code : rule.error_codes())
      require(identifier(code) && code.size() <= 128 && codes.insert(code).second,
              "runtime_argument_invalid");
  }
  for (const auto &rule : options.schedule_rules())
    require(ids.insert(rule.rule_id()).second, "runtime_rule_id_duplicate");
  v1::Level level = v1::L0;
  if (options.requested_level() >= v1::L1 &&
      contains(options.host_capabilities(), "context.request"))
    level = v1::L1;
  if (options.requested_level() >= v1::L2 && contains(options.host_capabilities(), "action.filter"))
    level = v1::L2;
  if (options.requested_level() >= v1::L2 &&
      contains(options.host_capabilities(), "schedule.dispatch"))
    level = v1::L2;
  if (options.requested_level() >= v1::L3 && contains(options.host_capabilities(), "loop.decide"))
    level = v1::L3;
  const auto needed = options.requested_level() == v1::L3   ? "loop.decide"
                      : options.requested_level() == v1::L2 ? "action.filter"
                                                            : "context.request";
  require(options.fallback() != "strict" || contains(options.host_capabilities(), needed) ||
              (options.requested_level() == v1::L2 &&
               contains(options.host_capabilities(), "schedule.dispatch")),
          "runtime_host_capability_missing");
  require(options.fallback() != "strict" || level >= options.requested_level(),
          "runtime_host_capability_missing");
  return std::min(level, options.requested_level());
}

// 记录当前执行缺少的能力或证据，明确说明为什么不能应用控制。
bool missing(const google::protobuf::Struct &arguments, const std::string &key) {
  const auto it = arguments.fields().find(key);
  if (it == arguments.fields().end())
    return true;
  const auto &value = it->second;
  return value.kind_case() == google::protobuf::Value::kNullValue ||
         value.kind_case() == google::protobuf::Value::KIND_NOT_SET ||
         (value.kind_case() == google::protobuf::Value::kListValue &&
          value.list_value().values().empty()) ||
         (value.kind_case() == google::protobuf::Value::kStructValue &&
          value.struct_value().fields().empty()) ||
         (value.kind_case() == google::protobuf::Value::kStringValue &&
          value.string_value().empty());
}

// 包装当前结果为标准结构，保留宿主结果与策略决定的区分。
std::string wrap(const std::string &, const std::string &content) {
  // 版本归因保留在 decision／receipt，不把每个 run 的编号混入模型正文。
  // 无关控制规则变化不能改变相同经验的输入，也不应破坏提示词缓存。
  return "\n<trajectory-experience>\n" + content + "\n</trajectory-experience>";
}

// 检查请求中是否已包含目标注入内容，避免重复增强。
bool request_contains(const google::protobuf::Struct &request, const std::string &context) {
  const auto it = request.fields().find("messages");
  if (it == request.fields().end() || it->second.kind_case() != google::protobuf::Value::kListValue)
    return false;
  unsigned count = 0;
  for (const auto &message : it->second.list_value().values()) {
    if (message.kind_case() != google::protobuf::Value::kStructValue)
      continue;
    const auto &fields = message.struct_value().fields();
    const auto role = fields.find("role"), content = fields.find("content");
    if (role == fields.end() || role->second.string_value() != "system" || content == fields.end())
      continue;
    const auto &text = content->second.string_value();
    for (std::size_t pos = 0; (pos = text.find(context, pos)) != std::string::npos;
         pos += context.size())
      ++count;
  }
  return count == 1;
}
} // 匿名命名空间

bool disjoint_failure_recovery_rules(const v1::LoopRule &first, const v1::LoopRule &second) {
  if (!first.has_source_experience() || !second.has_source_experience() ||
      first.target_scope() != second.target_scope() || first.actor_id() != second.actor_id() ||
      !first.error_codes_size() || !second.error_codes_size() ||
      first.match_mode() != "predicate" || second.match_mode() != "predicate" ||
      !first.replan_on_failure() || !second.replan_on_failure() ||
      first.max_replans() != 1 || second.max_replans() != 1 ||
      first.max_iterations() != second.max_iterations() ||
      first.max_no_progress() != second.max_no_progress() ||
      first.no_progress_decision() != second.no_progress_decision() ||
      first.after_action_rejection() != second.after_action_rejection() ||
      first.require_finalization() != second.require_finalization())
    return false;
  for (const auto &code : first.error_codes())
    if (contains(second.error_codes(), code))
      return false;
  return true;
}

bool has_multi_failure_recovery(const v1::RuntimeOptions &options, const std::string &scope,
                                const std::string &actor) {
  std::map<std::string, unsigned> counts;
  for (const auto &rule : options.loop_rules())
    if (rule.has_source_experience() && rule.target_scope() == scope &&
        (rule.actor_id().empty() || rule.actor_id() == actor) && ++counts[rule.actor_id()] > 1)
      return true;
  return false;
}

// 编译阶段把经验快照和经校验的选项固定为一个不可变策略版本；后续运行时只读取该版本。
// 同一 policy_id 在不同打点处共享同一套 L1、L2、L3 规则，具体决定仍取决于当前查询。
// 将快照与批准项编译成固定版本，校验能力等级、作用域和内容版本。
v1::RuntimePolicy compile_runtime_policy(v1::ShadowPolicySnapshot snapshot,
                                         const v1::RuntimeOptions &options) {
  require(snapshot.schema_version() == 2, "runtime_experience_contract_missing");
  ShadowRuntime checked(snapshot);
  ensure_safe_evolution_data(options);
  v1::RuntimePolicy policy;
  policy.set_schema_version(1);
  *policy.mutable_snapshot() = std::move(snapshot);
  *policy.mutable_options() = options;
  policy.set_effective_level(validate_options(options));
  const auto &identity = policy.snapshot().identity();
  for (const auto &disabled : options.disabled_injections()) {
    bool matched = false;
    for (const auto &source : policy.snapshot().experiences())
      matched |= source.control_target_type().empty() &&
                 source.memory_id() == disabled.memory_id() &&
                 source.content_revision() == disabled.content_revision();
    require(matched, "runtime_disabled_injection_source_invalid");
  }
  for (const auto &item : options.experience_overrides()) {
    bool matched = false;
    for (const auto &source : policy.snapshot().experiences())
      if (source.control_target_type().empty() &&
          source.memory_id() == item.source_experience().memory_id() &&
          source.content_revision() == item.source_experience().content_revision()) {
        matched = agentflow::trajectory::experience_fields_valid(
            source.target_scope(),
            item.trigger_pattern().empty() ? source.trigger_pattern() : item.trigger_pattern(),
            item.content());
      }
    require(matched, "runtime_experience_override_source_invalid");
  }
  require(contains(options.allowed_users(), identity.user_id()) &&
              (options.allowed_conversations().empty() ||
               contains(options.allowed_conversations(), identity.conversation_id())),
          "runtime_identity_not_authorized");
  for (const auto &rule : options.action_rules())
    require(contains(policy.snapshot().options().target_scopes(), rule.target_scope()),
            "runtime_scope_invalid");
  for (const auto &rule : options.loop_rules())
    require(contains(policy.snapshot().options().target_scopes(), rule.target_scope()),
            "runtime_scope_invalid");
  for (const auto &rule : options.schedule_rules())
    require(contains(policy.snapshot().options().target_scopes(), rule.target_scope()),
            "runtime_scope_invalid");
  require(policy.ByteSizeLong() <= 1024U * 1024U, "runtime_policy_too_large");
  policy.set_policy_id(revision(policy));
  return policy;
}

// 初始化 PolicyRuntime 的状态和资源；输入参数的校验与所有权在构造阶段确定。
// 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
PolicyRuntime::PolicyRuntime(v1::RuntimePolicy policy)
    : policy_(std::move(policy)), shadow_(policy_.snapshot()) {
  require(policy_.schema_version() == 1 && policy_.policy_id() == revision(policy_) &&
              compile_runtime_policy(policy_.snapshot(), policy_.options()).effective_level() ==
                  policy_.effective_level(),
          "runtime_policy_invalid");
  for (const auto &e : policy_.snapshot().experiences())
    for (const auto &a : policy_.options().approved_experiences())
      if (e.memory_id() == a.memory_id() && e.content_revision() == a.content_revision())
        approved_ids_.insert(e.memory_id());
}

// 固定策略解释打点查询；仅保留同一 run 的一次 L1 注入及其原准备决定。
// 业务状态只能在宿主随后提交实际执行结果时推进，不能把策略建议误记为已发生事实。
// 运行时读取当前边界事实并产生有界决策；这里不调用远程模型进行语义裁判。
v1::RuntimeDecision PolicyRuntime::decide(const v1::RuntimeQuery &query) const {
  ensure_safe_evolution_data(query);
  const auto &run_id = query.advice_query().identity().run_id();
  if (run_id != seen_run_id_) {
    seen_run_id_ = run_id;
    injected_experience_ids_.clear();
    prepared_injections_.clear();
  }
  const PreparedQueryKey prepared_key{run_id, query.advice_query().query_id(),
                                      query.advice_query().decision_id(), query.boundary()};
  const auto prepared = prepared_injections_.find(prepared_key);
  if (prepared != prepared_injections_.end() &&
      google::protobuf::util::MessageDifferencer::Equals(prepared->second.query(), query))
    return prepared->second;
  require(query.observed_operations_size() <= 64 && query.error_codes_size() <= 64 &&
              query.rejected_actions() <= 10000,
          "runtime_query_invalid");
  for (const auto &operation : query.observed_operations())
    require(identifier(operation), "runtime_query_invalid");
  for (const auto &code : query.error_codes())
    require(identifier(code) && code.size() <= 128, "runtime_query_invalid");
  auto eligible = approved_ids_;
  // An explicit model-boundary allowlist can only narrow the frozen approvals.
  // Its envelope is separate from retrieval text and cannot become anchors.
  std::string retrieval_state = query.advice_query().state();
  const auto first_state = retrieval_state.find_first_not_of(" \t\r\n");
  if (query.boundary() == "llm" && first_state != std::string::npos && retrieval_state[first_state] == '{' &&
      retrieval_state.find("runtime_experience_allowlist") != std::string::npos) {
    const auto envelope = integration::Json::parse(retrieval_state);
    if (envelope.object() && envelope.has("runtime_experience_allowlist")) {
      integration::keys(envelope, {"retrieval_state", "runtime_experience_allowlist"});
      const auto &refs = envelope["runtime_experience_allowlist"];
      require(refs.array() && refs.items().size() <= static_cast<std::size_t>(
                  policy_.options().approved_experiences_size()) && envelope["retrieval_state"].string(),
              "runtime_experience_allowlist_invalid");
      std::set<std::string> allowed;
      for (const auto &ref : refs.items()) {
        integration::keys(ref, {"memory_id", "content_revision"});
        bool approved = false;
        for (const auto &source : policy_.options().approved_experiences())
          approved |= source.memory_id() == ref.str("memory_id") &&
                      source.content_revision() == ref.str("content_revision");
        require(approved && allowed.insert(ref.str("memory_id")).second,
                "runtime_experience_allowlist_source_invalid");
      }
      for (auto it = eligible.begin(); it != eligible.end();)
        if (!allowed.count(*it)) it = eligible.erase(it); else ++it;
      retrieval_state = envelope["retrieval_state"].text();
    }
  }
  // L1 停用仅改变文字注入；动作、编排与循环仍使用原来源批准。
  if (query.boundary() == "llm")
    for (const auto &disabled : policy_.options().disabled_injections()) {
      eligible.erase(disabled.memory_id());
      for (const auto &relation : policy_.snapshot().cross_relations())
        if (relation.relation() == "duplicate" &&
            relation.private_memory_id() == disabled.memory_id() &&
            relation.private_content_revision() == disabled.content_revision())
          eligible.erase(relation.shared_memory_id());
    }
  std::map<std::string, std::string> triggers;
  for (const auto &rewrite : policy_.options().experience_overrides()) {
    if (query.boundary() != "llm")
      break; // L1 的适用条件只影响经验注入，不能削弱可执行控制。
    const auto &id = rewrite.source_experience().memory_id();
    if (!rewrite.trigger_pattern().empty()) {
      triggers[id] = rewrite.trigger_pattern();
      if (!actor_condition_matches(rewrite.trigger_pattern(), query.actor_id()))
        eligible.erase(id);
    }
    if (!rewrite.observed_operations().empty() &&
        std::none_of(rewrite.observed_operations().begin(), rewrite.observed_operations().end(),
                     [&](const auto &operation) {
                       return contains(query.observed_operations(), operation);
                     }))
      eligible.erase(id);
    // 当前版本若收窄私有规范记录，已被遮蔽的公共副本不能恢复旧条件或旧正文。
    for (const auto &relation : policy_.snapshot().cross_relations())
      if (relation.relation() == "duplicate" && relation.private_memory_id() == id &&
          relation.private_content_revision() == rewrite.source_experience().content_revision())
        eligible.erase(relation.shared_memory_id());
  }
  // 三类边界共用身份、作用域和输入预算校验，保证检索的适用口径一致。
  // AdviceQuery predates structured RuntimeQuery error fields.  Carry the
  // bounded, exact host error codes into its state so L1 retrieval can branch
  // on TypeError/401/422 without relying on redacted natural language.
  auto advice_query = query.advice_query();
  advice_query.set_state(retrieval_state);
  if (query.boundary() == "llm" && query.error_codes_size()) {
    auto state = advice_query.state();
    for (const auto &code : query.error_codes())
      if (identifier(code))
        state += "\nobserved_error_code:" + code;
    advice_query.set_state(state);
  }
  const auto advice = shadow_.query_advice(advice_query, eligible, triggers);
  require(advice.status() != "error" && identifier(query.execution_id()) &&
              query.ByteSizeLong() <= 128U * 1024U && query.actions_size() <= 128,
          "runtime_query_invalid");
  require(query.boundary() == "llm" || query.boundary() == "actions" ||
              query.boundary() == "schedule" || query.boundary() == "loop",
          "runtime_boundary_invalid");
  v1::RuntimeDecision result;
  result.set_policy_id(policy_.policy_id());
  *result.mutable_query() = query;
  result.set_status("no_change");
  const auto &options = policy_.options();
  auto source_matches = [&](const auto &rule) {
    if (!rule.has_source_experience())
      return true;
    // 可执行规则的来源仍须版本固定且已批准；不能因为无关 L1 建议占满 top-k
    // 就让这条规则的条件消失。
    if (rule.match_mode() == "predicate" || options.aggressive_controls())
      return approved_ids_.count(rule.source_experience().memory_id()) != 0;
    return std::any_of(advice.advice().begin(), advice.advice().end(), [&](const auto &item) {
      return item.experience().memory_id() == rule.source_experience().memory_id() &&
             item.experience().content_revision() == rule.source_experience().content_revision();
    });
  };
  // L1 只在模型请求边界生成经验上下文；动作、编排和循环控制均不依赖提示词命中。
  if (query.boundary() == "llm" && policy_.effective_level() >= v1::L1 &&
      contains(options.host_capabilities(), "context.request")) {
    std::string content;
    std::set<std::string> applied;
    auto append_context = [&](const agentflow::trajectory::evaluation::ExperienceAdvice &item) {
      auto next = content + (content.empty() ? "" : ",\n") + to_json(item, false);
      auto rendered = options.experience_template();
      rendered.replace(rendered.find("{{experience}}"), 14, "[" + next + "]");
      rendered = wrap(policy_.policy_id(), rendered);
      if (rendered.size() > options.max_context_bytes())
        return false;
      content = std::move(next);
      result.set_context(std::move(rendered));
      return true;
    };
    // 编排规则只在 schedule 边界改写真实批次，不转成模型提示。
    // 否则同一规则先改变模型的分批方式，可能消除后续确定性调度的应用机会，
    // 还会把只修改 L2 的实验混入额外上下文变化，无法区分各级策略的实际作用。
    std::set<std::pair<std::string, std::string>> injected_content;
    for (const auto &match : advice.advice()) {
      const auto &experience = match.experience();
      // Only the original prepared query may replay its injection. A later
      // query remains unchanged even if its history contains earlier advice.
      if (injected_experience_ids_.count(experience.memory_id()))
        continue;
      // 优先采用已批准的私有重复记录；相关或独立建议仍可补充。若无私有批准记录，
      // 公共副本才可入选。
      if (experience.application_shared() &&
          std::any_of(policy_.snapshot().cross_relations().begin(),
                      policy_.snapshot().cross_relations().end(), [&](const auto &r) {
                        return r.relation() == "duplicate" &&
                               r.shared_memory_id() == experience.memory_id() &&
                               applied.count(r.private_memory_id());
                      }))
        continue;
      auto approved = std::find_if(options.approved_experiences().begin(),
                                   options.approved_experiences().end(), [&](const auto &a) {
                                     return a.memory_id() == experience.memory_id() &&
                                            a.content_revision() == experience.content_revision();
                                   });
      if (approved == options.approved_experiences().end())
        continue;
      if (!agentflow::trajectory::experience_fields_valid(
              experience.target_scope(), experience.trigger_pattern(), experience.content()))
        continue;
      // 有类型 JSON 保留字段顺序，使 verify() 重算时得到相同的上下文。
      agentflow::trajectory::evaluation::ExperienceAdvice item;
      item.set_target_scope(experience.target_scope());
      item.set_trigger_pattern(experience.trigger_pattern());
      item.set_proposed_change(experience.content());
      for (const auto &rewrite : options.experience_overrides())
        if (rewrite.source_experience().memory_id() == approved->memory_id() &&
            rewrite.source_experience().content_revision() == approved->content_revision()) {
          item.set_proposed_change(rewrite.content());
          // 检索与注入使用同一版本的适用条件，不能展示来源记忆的旧条件。
          if (!rewrite.trigger_pattern().empty())
            item.set_trigger_pattern(rewrite.trigger_pattern());
        }
      // 同一有效建议可能来自不同私有/公共记录；一次请求只注入一份。
      const auto content_key = std::make_pair(item.target_scope(), item.proposed_change());
      if (injected_content.count(content_key) || !append_context(item))
        continue;
      injected_content.insert(content_key);
      injected_experience_ids_.insert(experience.memory_id());
      *result.add_experiences() = *approved;
      applied.insert(experience.memory_id());
    }
    if (!result.context().empty())
      result.set_status("ready");
  } else if (query.boundary() == "actions") {
    // L2 在动作真正派发前逐项给出准入结论；拒绝不等价于一次已执行的业务失败。
    std::set<std::string> action_ids;
    for (const auto &action : query.actions()) {
      require(identifier(action.action_id()) && identifier(action.operation()) &&
                  action_ids.insert(action.action_id()).second,
              "runtime_action_id_invalid");
      std::set<std::string> schema_required;
      require(action.schema_required_arguments_size() <= 32, "runtime_action_schema_invalid");
      for (const auto &key : action.schema_required_arguments())
        require(identifier(key) && schema_required.insert(key).second,
                "runtime_action_schema_invalid");
      auto *decision = result.add_actions();
      decision->set_action_id(action.action_id());
      decision->set_decision("allow");
      if (policy_.effective_level() < v1::L2 ||
          !contains(options.host_capabilities(), "action.filter"))
        continue;
      for (const auto &rule : options.action_rules()) {
        const bool schema_rule = rule.operation() == kSchemaOperation &&
                                 rule.required_arguments_size() == 1 &&
                                 rule.required_arguments(0) == kSchemaRequiredNonempty;
        const bool repeat_rule =
            rule.max_failed_attempts() > 0 && rule.required_arguments_size() == 0;
        if (rule.target_scope() != query.advice_query().target_scope() ||
            (!schema_rule && rule.operation() != kSchemaOperation &&
             rule.operation() != action.operation()) ||
            !source_matches(rule))
          continue;
        const auto target = action.target_failures().find(rule.rule_id());
        const auto failures = rule.failure_key_arguments_size() ||
                                      rule.terminal_error_codes_size() || rule.max_attempts()
                                  ? (target == action.target_failures().end() ? 0 : target->second)
                                  : action.prior_failures();
        bool reject = rule.deny() || (repeat_rule && failures >= rule.max_failed_attempts()) ||
                      (rule.max_attempts() && failures >= rule.max_attempts());
        if (schema_rule) {
          for (const auto &key : action.schema_required_arguments())
            reject |= missing(action.arguments(), key);
        } else
          for (const auto &key : rule.required_arguments())
            reject |= missing(action.arguments(), key);
        if (!reject)
          continue;
        decision->set_decision("reject");
        decision->set_rule_id(rule.rule_id());
        decision->set_reason(rule.reason());
        result.set_status("ready");
        if (rule.has_source_experience())
          *result.add_experiences() = rule.source_experience();
        break;
      }
    }
  } else if (query.boundary() == "schedule") {
    // L2 编排仅描述依赖和顺序，实际并发/串行状态仍由宿主的 schedule.observe 打点确认。
    auto effective = options;
    effective.set_requested_level(policy_.effective_level());
    *result.mutable_schedule() = plan_schedule(effective, query);
    result.mutable_schedule()->set_policy_id(policy_.policy_id());
    if (result.schedule().status() == "scheduled") {
      result.set_status("ready");
      for (const auto &id : result.schedule().rule_ids())
        for (const auto &rule : options.schedule_rules())
          if (rule.rule_id() == id && rule.has_source_experience())
            *result.add_experiences() = rule.source_experience();
    }
  } else if (query.boundary() == "loop") {
    // L3 根据宿主已经观察到的循环状态作出 continue/replan/stop 建议，不能越过在途动作。
    require(query.iteration() > 0 && query.state_fingerprint().size() <= 128,
            "runtime_iteration_invalid");
    const auto &previous = query.previous();
    require((previous.execution_id().empty() && previous.iteration() == 0) ||
                (previous.execution_id() == query.execution_id() &&
                 previous.iteration() <= query.iteration() &&
                 previous.policy_id() == policy_.policy_id() &&
                 previous.target_scope() == query.advice_query().target_scope()),
            "runtime_loop_state_invalid");
    auto *progress = result.mutable_progress();
    *progress = previous;
    if (query.pending_actions() || policy_.effective_level() < v1::L3 ||
        !contains(options.host_capabilities(), "loop.decide")) {
      // 宿主仍持有在途动作时，绝不能停止或重规划。
      progress->set_decision("continue");
      return result;
    }
    const v1::LoopRule *rule = nullptr;
    const bool multi = has_multi_failure_recovery(options, query.advice_query().target_scope(),
                                                  query.actor_id());
    for (const auto &candidate : options.loop_rules())
      if (candidate.target_scope() == query.advice_query().target_scope() &&
          source_matches(candidate) &&
          (!multi || !candidate.has_source_experience() ||
           (query.error_codes_size() > 0 &&
            std::all_of(query.error_codes().begin(), query.error_codes().end(),
                        [&](const auto &code) { return contains(candidate.error_codes(), code); }))) &&
          (candidate.actor_id().empty() || candidate.actor_id() == query.actor_id()) &&
          (!candidate.require_finalization() || query.can_finalize()) &&
          (!candidate.after_action_rejection() || query.rejected_actions()) &&
          (candidate.error_codes().empty() ||
           std::any_of(candidate.error_codes().begin(), candidate.error_codes().end(),
                       [&](const auto &code) { return contains(query.error_codes(), code); })) &&
          (!rule || (rule->actor_id().empty() && !candidate.actor_id().empty()) ||
           (rule->actor_id() == candidate.actor_id() && candidate.has_source_experience())))
        rule = &candidate;
    // 同一轮次只能重放同一状态指纹，保证重试幂等且不会吞掉并发分支的新结果。
    if (previous.iteration() == query.iteration()) {
      require(previous.fingerprint() == query.state_fingerprint(),
              "runtime_duplicate_iteration_changed");
    } else {
      progress->set_execution_id(query.execution_id());
      progress->set_policy_id(policy_.policy_id());
      progress->set_target_scope(query.advice_query().target_scope());
      progress->set_iteration(query.iteration());
      progress->set_fingerprint(query.state_fingerprint());
      if (previous.decision() != "stop") {
        progress->set_decision("continue");
        progress->clear_rule_id();
        progress->clear_reason();
        progress->set_no_progress(
            std::max({query.repeated_action_steps(),
                      options.aggressive_controls() ? query.repeated_failure_steps() : 0U,
                      !query.state_fingerprint().empty() && previous.iteration() > 0 &&
                              previous.fingerprint() == query.state_fingerprint()
                          ? previous.no_progress() + 1
                          : 0U}));
        if (rule) {
          if (rule->max_iterations() && query.iteration() > rule->max_iterations()) {
            progress->set_decision("stop");
          } else if (rule->max_no_progress() &&
                     progress->no_progress() >= rule->max_no_progress()) {
            if (rule->no_progress_decision() == "replan" &&
                progress->replans() < rule->max_replans()) {
              progress->set_decision("replan");
              progress->set_replans(progress->replans() + 1);
              progress->set_no_progress(0);
            } else
              progress->set_decision("stop");
          } else if (rule->replan_on_failure() &&
                     (rule->has_source_experience() ? query.repeated_failure_steps()
                                                    : query.failed_actions()) &&
                     progress->replans() < rule->max_replans()) {
            // 学习得到的恢复规则必须看到重复的结构化失败；单次动作失败不足以证明
            // 再开一轮循环会有收益。人工基线规则仍保留其显式失败动作语义。
            progress->set_decision("replan");
            progress->set_replans(progress->replans() + 1);
          }
          if (progress->decision() != "continue") {
            progress->set_rule_id(rule->rule_id());
            progress->set_reason(rule->reason());
          }
        }
      }
    }
    if (progress->decision() != "continue") {
      result.set_status("ready");
      if (rule && rule->has_source_experience())
        *result.add_experiences() = rule->source_experience();
      if (progress->decision() == "replan") {
        require(rule != nullptr, "runtime_replan_rule_missing");
        result.set_context(wrap(policy_.policy_id(), rule->replan_context()));
        require(result.context().size() <= options.max_context_bytes(),
                "runtime_replan_context_too_large");
      }
    }
  }
  // Every retained L1 decision consumed at least one previously unused
  // approved experience, so this run-local cache is naturally bounded by
  // the approved experience count. No unchanged query or control is stored.
  if (query.boundary() == "llm" && !result.context().empty())
    prepared_injections_[prepared_key] = result;
  return result;
}

// 重新校验策略决定与实际输入、回执是否一致，不直接相信调用方声称已应用。
v1::RuntimeReceipt PolicyRuntime::verify(const v1::RuntimeQuery &query,
                                         const v1::RuntimeObservation &observation) const {
  return verify(query, observation, policy_.options());
}

// 重新校验策略决定与实际输入、回执是否一致，不直接相信调用方声称已应用。
v1::RuntimeReceipt PolicyRuntime::verify(const v1::RuntimeQuery &query,
                                         const v1::RuntimeObservation &observation,
                                         const v1::RuntimeOptions &request_binding) const {
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
  auto *application = receipt.mutable_application();
  application->set_query_id(query.advice_query().query_id());
  application->set_decision_id(query.advice_query().decision_id());
  application->set_policy_id(policy_.policy_id());
  application->set_policy_revision(policy_.policy_id());
  bool applied = false;
  if (decision.status() == "ready") {
    if (!decision.context().empty()) {
      applied = observation.transport_allowed() && identifier(observation.call_id()) &&
                (request_binding.request_context_pointer().empty()
                     ? request_contains(observation.request(), decision.context())
                     : bound_request_contains(observation.request(), decision.context(),
                                              request_binding));
    } else if (query.boundary() == "actions") {
      applied = true;
      for (const auto &action : decision.actions()) {
        if (action.decision() != "reject")
          continue;
        receipt.add_action_ids(action.action_id());
        applied &= contains(observation.skipped_action_ids(), action.action_id()) &&
                   !contains(observation.executed_action_ids(), action.action_id());
      }
    } else if (query.boundary() == "schedule") {
      try {
        auto state = begin_schedule(decision.schedule());
        require(observation.schedule_events_size() <= 512, "schedule_observation_limit");
        for (const auto &event : observation.schedule_events())
          state = advance_schedule(std::move(state), event.action_id(), event.status());
        applied = schedule_finished(state);
        for (const auto &action : state.actions()) {
          if (action.status() == "skipped")
            applied &= contains(observation.skipped_action_ids(), action.action_id()) &&
                       !contains(observation.executed_action_ids(), action.action_id());
          else
            applied &= contains(observation.executed_action_ids(), action.action_id());
        }
        if (applied)
          for (const auto &id : decision.schedule().rule_ids())
            receipt.add_rule_ids(id);
      } catch (const agentflow::trajectory::EvaluationError &) {
        applied = false;
      }
    } else if (query.boundary() == "loop") {
      // A host-confirmed replan is a real loop application as well.  The
      // previous stop-only check marked every successful replan as failed,
      // under-counting L3 receipts.
      const auto proposed = decision.progress().decision();
      const auto observed = observation.loop_decision();
      applied = ((proposed == "stop" && observed == "stop") ||
                 (proposed == "replan" && observed == "replan")) &&
                !query.pending_actions();
    }
  }
  application->set_status(applied ? "applied"
                                  : (decision.status() == "ready" ? "failed" : "rejected"));
  application->set_reason(applied ? "host_boundary_verified" : "application_not_observed");
  return receipt;
}

} // trajectory::evolution 命名空间
