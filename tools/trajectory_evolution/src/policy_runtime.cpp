#include "trajectory_evolution/policy_runtime.h"
#include "trajectory_evolution/trace.h"
#include "trajectory_evolution/request_binding.h"
#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/experience.h"

#include <algorithm>
#include <array>
#include <set>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/sha.h>

namespace trajectory::evolution {
namespace {
using agentflow::trajectory::ensure_safe_evolution_data;

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
  require(options.requested_level() >= v1::L1 && options.requested_level() <= v1::L3,
          "runtime_level_invalid");
  require(options.fallback() == "strict" || options.fallback() == "downgrade",
          "runtime_fallback_invalid");
  require(options.allowed_users_size() > 0 && options.allowed_users_size() <= 256
              && options.allowed_conversations_size() <= 256
              && options.approved_experiences_size() <= 512
              && options.experience_overrides_size() <= 64
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
  for(const auto& item : options.experience_overrides()) {
    require(approved_source(item.source_experience())
                && ids.insert(item.source_experience().memory_id()).second
                && !item.content().empty() && item.content().size() <= options.max_context_bytes(),
            "runtime_experience_override_invalid");
  }
  ids.clear();
  for(const auto& rule : options.action_rules()) {
    require(!rule.has_source_experience() || approved_source(rule.source_experience()), "runtime_rule_source_not_approved");
    require(identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second
                && identifier(rule.target_scope()) && identifier(rule.operation())
                && !rule.reason().empty() && rule.reason().size() <= 1024
                && (rule.deny() || rule.required_arguments_size() > 0)
                && rule.required_arguments_size() <= 32, "runtime_action_rule_invalid");
    for(const auto& key : rule.required_arguments()) require(identifier(key), "runtime_argument_invalid");
  }
  std::set<std::string> loop_scopes;
  for(const auto& rule : options.loop_rules()) {
    require(!rule.has_source_experience() || approved_source(rule.source_experience()), "runtime_rule_source_not_approved");
    require(identifier(rule.rule_id()) && ids.insert(rule.rule_id()).second
                && identifier(rule.target_scope()) && loop_scopes.insert(rule.target_scope() + (rule.has_source_experience() ? ":generated" : ":base")).second
                && (rule.max_iterations() > 0 || rule.max_no_progress() > 0)
                && rule.max_iterations() <= 10000 && rule.max_no_progress() <= 10000
                && rule.max_replans() <= 16 && !rule.reason().empty() && rule.reason().size() <= 1024
                && (rule.no_progress_decision() == "stop" || rule.no_progress_decision() == "replan")
                && (rule.no_progress_decision() != "replan" || (rule.max_replans() > 0
                    && !rule.replan_context().empty()
                    && rule.replan_context().size() + 128 <= options.max_context_bytes())),
            "runtime_loop_rule_invalid");
  }
  v1::Level level = v1::L0;
  if(options.requested_level() >= v1::L1 && contains(options.host_capabilities(), "context.request")) level = v1::L1;
  if(options.requested_level() >= v1::L2 && contains(options.host_capabilities(), "action.filter")) level = v1::L2;
  if(options.requested_level() >= v1::L3 && contains(options.host_capabilities(), "loop.decide")) level = v1::L3;
  const auto needed = options.requested_level() == v1::L3 ? "loop.decide"
      : options.requested_level() == v1::L2 ? "action.filter" : "context.request";
  require(options.fallback() != "strict" || contains(options.host_capabilities(), needed),
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
std::string wrap(const std::string& policy_id, const std::string& content) {
  return "\n<trajectory-experience policy=\"" + policy_id + "\">\n" + content
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
}  // namespace

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
  for(const auto& item : options.experience_overrides()) {
    bool matched = false;
    for(const auto& source : policy.snapshot().experiences())
      if(source.memory_id() == item.source_experience().memory_id()
          && source.content_revision() == item.source_experience().content_revision()) {
        matched = agentflow::trajectory::experience_fields_valid(
            source.target_scope(), source.trigger_pattern(), item.content());
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

v1::RuntimeDecision PolicyRuntime::decide(const v1::RuntimeQuery& query) const {
  ensure_safe_evolution_data(query);
  // Reuse identity, scope and input budget checks for all three boundaries.
  const auto advice = shadow_.query_advice(query.advice_query(), approved_ids_);
  require(advice.status() != "error" && identifier(query.execution_id())
              && query.ByteSizeLong() <= 128U * 1024U && query.actions_size() <= 128,
          "runtime_query_invalid");
  require(query.boundary() == "llm" || query.boundary() == "actions" || query.boundary() == "loop",
          "runtime_boundary_invalid");
  v1::RuntimeDecision result;
  result.set_policy_id(policy_.policy_id());
  *result.mutable_query() = query;
  result.set_status("no_change");
  const auto& options = policy_.options();
  auto source_matches = [&](const auto& rule) {
    if(!rule.has_source_experience()) return true;
    return std::any_of(advice.advice().begin(), advice.advice().end(), [&](const auto& item) {
      return item.experience().memory_id() == rule.source_experience().memory_id()
          && item.experience().content_revision() == rule.source_experience().content_revision();
    });
  };
  if(query.boundary() == "llm" && policy_.effective_level() >= v1::L1
      && contains(options.host_capabilities(), "context.request")) {
    std::string content;
    std::set<std::string> applied;
    for(const auto& match : advice.advice()) {
      const auto& experience = match.experience();
      // Prefer an approved private duplicate; related/independent advice may
      // still complement it. No private approval means the public copy can win.
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
      // Typed JSON preserves field order when verify() recomputes the same context.
      agentflow::trajectory::evaluation::ExperienceAdvice item;
      item.set_target_scope(experience.target_scope());
      item.set_trigger_pattern(experience.trigger_pattern());
      item.set_proposed_change(experience.content());
      for(const auto& rewrite : options.experience_overrides())
        if(rewrite.source_experience().memory_id() == approved->memory_id()
            && rewrite.source_experience().content_revision() == approved->content_revision())
          item.set_proposed_change(rewrite.content());
      auto next = content + (content.empty() ? "" : ",\n") + to_json(item, false);
      auto rendered = options.experience_template();
      rendered.replace(rendered.find("{{experience}}"), 14, "[" + next + "]");
      rendered = wrap(policy_.policy_id(), rendered);
      if(rendered.size() > options.max_context_bytes()) continue;
      content = std::move(next);
      result.set_context(std::move(rendered));
      *result.add_experiences() = *approved;
      applied.insert(experience.memory_id());
    }
    if(!result.context().empty()) result.set_status("ready");
  } else if(query.boundary() == "actions") {
    std::set<std::string> action_ids;
    for(const auto& action : query.actions()) {
      require(identifier(action.action_id()) && identifier(action.operation())
                  && action_ids.insert(action.action_id()).second, "runtime_action_id_invalid");
      auto* decision = result.add_actions();
      decision->set_action_id(action.action_id());
      decision->set_decision("allow");
      if(policy_.effective_level() < v1::L2 || !contains(options.host_capabilities(), "action.filter")) continue;
      for(const auto& rule : options.action_rules()) {
        if(rule.target_scope() != query.advice_query().target_scope() || rule.operation() != action.operation() || !source_matches(rule)) continue;
        bool reject = rule.deny();
        for(const auto& key : rule.required_arguments()) reject |= missing(action.arguments(), key);
        if(!reject) continue;
        decision->set_decision("reject");
        decision->set_rule_id(rule.rule_id());
        decision->set_reason(rule.reason());
        result.set_status("ready");
        if(rule.has_source_experience()) *result.add_experiences() = rule.source_experience();
        break;
      }
    }
  } else if(query.boundary() == "loop") {
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
      // Never stop/replan while the host still owns in-flight actions.
      progress->set_decision("continue");
      return result;
    }
    const v1::LoopRule* rule = nullptr;
    for(const auto& candidate : options.loop_rules())
      if(candidate.target_scope() == query.advice_query().target_scope() && source_matches(candidate)
          && (!rule || candidate.has_source_experience())) rule = &candidate;
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
        progress->set_no_progress(!query.state_fingerprint().empty() && previous.iteration() > 0
                && previous.fingerprint() == query.state_fingerprint() ? previous.no_progress() + 1 : 0);
        if(rule) {
          if(rule->max_iterations() && query.iteration() > rule->max_iterations()) {
            progress->set_decision("stop");
          } else if(rule->max_no_progress() && progress->no_progress() >= rule->max_no_progress()) {
            if(rule->no_progress_decision() == "replan" && progress->replans() < rule->max_replans()) {
              progress->set_decision("replan");
              progress->set_replans(progress->replans() + 1);
              progress->set_no_progress(0);
            } else progress->set_decision("stop");
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
    } else if(query.boundary() == "loop") {
      applied = decision.progress().decision() == "stop" && observation.loop_decision() == "stop"
          && !query.pending_actions();
    }
  }
  application->set_status(applied ? "applied" : (decision.status() == "ready" ? "failed" : "rejected"));
  application->set_reason(applied ? "host_boundary_verified" : "application_not_observed");
  return receipt;
}

}  // namespace trajectory::evolution
