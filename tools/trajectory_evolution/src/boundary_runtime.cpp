#include "boundary_runtime.h"
#include "model_contract.h"

#include "trajectory_evolution/evolution.h"
#include "trajectory_evolution/request_binding.h"

#include <chrono>
#include <set>
#include <openssl/sha.h>
#include <rapidjson/pointer.h>

namespace trajectory::evolution::integration {
namespace {
std::string hash(const std::string& value) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes), "hash_failed", 500);
  std::string result; constexpr char hex[] = "0123456789abcdef";
  for(auto b : bytes) { result += hex[b >> 4]; result += hex[b & 15]; }
  return result;
}
std::string component(const std::string& value) {
  std::string result; constexpr char hex[] = "0123456789ABCDEF";
  for(unsigned char c : value) {
    if((std::isalnum(c) && c < 128) || c == '-' || c == '_' || c == '.') result += c;
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
std::string qualified_scope(const Json& config, const std::string& scope) {
  return "agent:" + component(config.str("agent_id")) + "/actor:" + component(scope);
}
bool ended(const Json& value) { return value.flag("ended", false); }
std::string terminal(const Json& request) {
  const auto status = request.str("status");
  require(status == "completed" || status == "failed" || status == "cancelled" || status == "unknown", "status_invalid");
  return status;
}
Json extensions(const Json& request) {
  require(!request.has("extensions") || request["extensions"].object(), "extensions_object_required");
  return request.has("extensions") ? request["extensions"] : Json(Json::Object{});
}
std::string query_excerpt(const Json& value, std::size_t limit) {
  const auto text = value.string() ? value.text() : value.dump();
  if(text.size() <= limit) return text;
  const std::string marker = "\n[query excerpt]\n";
  std::size_t head = (limit - marker.size()) / 2, tail = text.size() - (limit - marker.size() - head);
  while(head && (static_cast<unsigned char>(text[head]) & 0xc0) == 0x80) --head;
  while(tail < text.size() && (static_cast<unsigned char>(text[tail]) & 0xc0) == 0x80) ++tail;
  return text.substr(0, head) + marker + text.substr(tail);
}
std::string scope_id(const Json& request, const Json& config) {
  const auto scope = request.str("scope_id", config["actors"].items().front().text());
  bool found = false; for(const auto& item : config["actors"].items()) found |= item.text() == scope;
  require(found, "policy_scope_not_authorized", 403); return scope;
}
Json event_context(const Json& item) {
  Json result{{"actor_id", item["actor_id"]}, {"execution_id", item["stream_id"]}, {"step_id", item["step_id"]}};
  if(item.has("loop_id")) { result["loop_id"] = item["loop_id"]; result["iteration"] = item["iteration"]; }
  return result;
}
v1::RuntimeOptions binding_options(const Json& binding) {
  keys(binding, {"pointer", "mode"});
  require(binding.has("pointer") && binding.has("mode"), "model_binding_required");
  const auto pointer = binding.str("pointer"), mode = binding.str("mode");
  require(pointer.empty() || pointer.front() == '/', "binding_pointer_invalid");
  v1::RuntimeOptions options;
  // A private envelope permits any JSON model argument, including a bare string.
  options.set_request_context_pointer("/payload" + pointer);
  options.set_request_context_mode(mode);
  validate_request_binding(options); return options;
}

std::string activity_query_state(const Json& request, const std::string& semantic, const Json& config, const Json& binding) {
  if(!request["state"].null()) return bounded_query_text(request["state"], config, false);
  if(semantic != "model" || binding.str("mode") != "system_message")
    return bounded_query_text(request["input"], config, false);
  // Follow the configured binding, not a provider-specific messages field.
  const auto path = binding.str("pointer");
  const rapidjson::Pointer pointer(path.c_str(), path.size());
  const Json* value = &request["input"];
  for(std::size_t i = 0; value && i < pointer.GetTokenCount(); ++i) {
    const auto& token = pointer.GetTokens()[i];
    if(value->object()) {
      const auto found = value->fields().find(std::string(token.name, token.length));
      value = found == value->fields().end() ? nullptr : &found->second;
    } else if(value->array() && token.index != rapidjson::kPointerInvalidIndex && token.index < value->items().size()) {
      value = &value->items()[token.index];
    } else value = nullptr;
  }
  Json messages = Json::Array{};
  if(value && value->array()) for(const auto& message : value->items()) {
    const auto& role = message["role"];
    // Static instructions, including earlier injections, are not observations.
    if(role.string() && (role.text() == "system" || role.text() == "developer")) continue;
    messages.push(message);
  }
  return bounded_query_text(messages, config, false);
}

class Runtime {
 public:
  Json& run;
  const Json& config;
  const PolicyRuntime* cached_policy;
  Runtime(Json& value, const Json& configuration, const PolicyRuntime* policy = nullptr)
      : run(value), config(configuration), cached_policy(policy) {}

  std::string emit(const std::string& kind, Json fields, const std::string& producer, Json causes = Json::Array{}) {
    require(run["events"].items().size() < config.num("max_events", 10000), "event_capacity_reached", 507);
    require(causes.items().size() <= 64, "too_many_causes");
    const auto id = "event-" + std::to_string(run["events"].items().size() + 1);
    const auto sequence = run["sequences"].num(producer, 0) + 1;
    run["sequences"][producer] = sequence;
    fields["event_id"] = id; fields["kind"] = kind; fields["producer_id"] = producer;
    if(!fields.has("actor_id")) fields["actor_id"] = "unspecified";
    fields["sequence"] = std::to_string(sequence);
    fields["timestamp_us"] = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if(!causes.items().empty()) fields["caused_by"] = causes;
    run["events"].push(fields); return id;
  }
  Json causes(const Json& request) const {
    Json result = Json::Array{}; std::set<std::string> unique;
    if(!request.has("caused_by")) return result;
    require(request["caused_by"].items().size() <= 64, "too_many_causes");
    for(const auto& id : request["caused_by"].items()) {
      bool exists = false; for(const auto& event : run["events"].items()) exists |= event.str("event_id") == id.text();
      require(exists, "causal_event_not_found", 409);
      if(unique.insert(id.text()).second) result.push(id);
    }
    return result;
  }
  Json join(Json links, const std::string& producer) {
    std::set<std::string> unique;
    auto& values = links.items();
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& id) { return !unique.insert(id.text()).second; }), values.end());
    while(links.items().size() > 64) {
      Json part = Json::Array{};
      for(unsigned i = 0; i < 64; ++i) { part.push(links.items().back()); links.items().pop_back(); }
      links.push(emit("custom", Json{{"attributes", Json{{"type", "boundary.join"}}}}, producer, part));
    }
    return links;
  }
  v1::RuntimeQuery query(const Json& item, const std::string& boundary, const std::string& id, const Json& state) const {
    v1::RuntimeQuery q;
    *q.mutable_advice_query()->mutable_identity() = proto<v1::Identity>(run["identity"]);
    q.mutable_advice_query()->set_target_scope(qualified_scope(config, item.str("scope_id")));
    q.mutable_advice_query()->set_query_id("query-" + id); q.mutable_advice_query()->set_decision_id("decision-" + id);
    if(run.num("level", 0) > 0) {
      // Only the retrieval view is bounded; the original request and trace stay intact.
      q.mutable_advice_query()->set_task(bounded_query_text(run["input"], config, true));
      q.mutable_advice_query()->set_state(bounded_query_text(state, config, false));
    }
    q.set_execution_id(item.str("stream_id")); q.set_boundary(boundary); return q;
  }
  v1::RuntimeDecision decide(const Json& item, v1::RuntimeQuery q) {
    const auto decision = cached_policy ? cached_policy->decide(q)
        : PolicyRuntime(proto<v1::RuntimePolicy>(run["policy"])).decide(q);
    auto fields = event_context(item); fields["attributes"] = Json{{"type", "policy.query"}, {"decision", json(decision)}};
    emit("custom", fields, item.str("step_id")); return decision;
  }
  void receipt(const Json& item, const Json& stored, const v1::RuntimeObservation& observed, const Json& binding = Json{}) {
    if(stored.null()) return;
    std::unique_ptr<PolicyRuntime> owned;
    if(!cached_policy) owned = std::make_unique<PolicyRuntime>(proto<v1::RuntimePolicy>(run["policy"]));
    const auto& runtime = cached_policy ? *cached_policy : *owned;
    const auto q = proto<v1::RuntimeQuery>(stored);
    const auto result = binding.null() ? runtime.verify(q, observed) : runtime.verify(q, observed, binding_options(binding));
    auto fields = event_context(item); fields["application"] = json(result.application());
    fields["attributes"] = Json{{"receipt", json(result)}};
    if(!binding.null()) fields["attributes"]["request_binding"] = binding;
    emit("advice.applied", fields, item.str("step_id"));
    run["receipt_counts"][result.application().status()] = run["receipt_counts"].num(result.application().status(), 0) + 1;
  }
  Json begin_activity(const Json& request) {
    keys(request, {"operation", "request_id", "run_id", "kind", "name", "input", "actor_id", "scope_id", "stream_id",
      "control_id", "state", "binding", "caused_by", "extensions", "apply_policy"});
    const bool apply_policy = request.flag("apply_policy", true);
    const auto kind = request.str("kind"), name = request.str("name"), id = "activity-" + hash(request.str("request_id")).substr(0, 28);
    require(identifier(kind) && identifier(name) && request.has("input"), "activity_fields_required");
    std::string semantic = kind == "model" || kind == "action" ? kind : "custom";
    if(config["activity_types"].has(kind)) {
      const auto& type = config["activity_types"][kind]; semantic = type.str("semantic");
      if(type.has("required_input_paths")) {
        rapidjson::Document data; const auto raw = request["input"].dump(); data.Parse(raw.data(), raw.size());
        for(const auto& path : type["required_input_paths"].items())
          require(rapidjson::Pointer(path.text().c_str()).Get(data) != nullptr, "required_input_field_missing");
      }
    }
    Json item{{"activity_id", id}, {"step_id", "step-" + id}, {"actor_id", request.str("actor_id", "unspecified")},
      {"scope_id", scope_id(request, config)}, {"stream_id", request.str("stream_id", id)}, {"kind", kind},
      {"semantic", semantic}, {"name", name}, {"input", request["input"]}, {"decision", "allow"}, {"ended", false}};
    require(identifier(item.str("actor_id")) && identifier(item.str("stream_id")), "activity_identity_invalid");
    Json links = causes(request);
    if(request.has("control_id")) {
      const auto control = request.str("control_id");
      require(run["controls"].has(control), "control_not_found", 404);
      const auto& c = run["controls"][control];
      require(c.flag("committed", false) && c.str("actual_decision") != "stop", "control_does_not_allow_activity", 409);
      require(!run["loops"][c.str("loop_id")].flag("stopped", false), "loop_already_stopped", 409);
      require(item.str("scope_id") == c.str("scope_id"), "control_scope_mismatch", 409);
      item["control_id"] = control; item["loop_id"] = c["loop_id"]; item["iteration"] = c["iteration"];
      if(!request.has("stream_id")) item["stream_id"] = c["stream_id"];
      links.push(c["commit_event"]);
    }
    // Keep tool contracts local to a task/branch. Neither another user's trace
    // nor a concurrent sibling can grant permissions to this activity.
    const auto& ref = request["extensions"]["point_reference"];
    const auto branch = ref.object() ? ref.str("parent", "root")
        : request.has("control_id") ? request.str("control_id") : request["caused_by"].dump();
    item["contract_scope"] = hash(item.str("scope_id") + "\n" + item.str("actor_id") + "\n" + branch);
    auto fields = event_context(item); fields["input"] = request["input"]; fields["operation"] = name;
    fields["attributes"] = Json{{"type", "boundary.activity"}, {"activity_kind", kind}, {"semantic", semantic},
      {"policy_scope", item["scope_id"]}, {"apply_policy", apply_policy}, {"extensions", extensions(request)}};
    item["start_event"] = emit("step.start", fields, item.str("step_id"), join(links, id));
    Json prepared = request["input"]; std::string reason;
    if(semantic == "model") {
      const auto binding = request.has("binding") ? request["binding"] : infer_model_binding(prepared);
      require(!apply_policy || run.num("level", 0) == 0 || !binding.null(), "model_binding_required");
      if(!binding.null()) { binding_options(binding); item["binding"] = binding; }
    }
    bool contract_failed = false;
    if(semantic == "action") {
      const auto& contracts = static_cast<const Json&>(run)["tool_contracts"][item.str("contract_scope")];
      item["contract_check"] = check_tool_contract(contracts, name, prepared);
      contract_failed = apply_policy && run.num("level", 0) >= 2 && item["contract_check"].str("status") == "failed";
      item["contract_check"]["enforced"] = contract_failed;
      if(contract_failed) { item["decision"] = "reject"; reason = item["contract_check"].str("reason") + ": " + item["contract_check"]["missing"].dump(); }
    }
    if(apply_policy && !contract_failed && semantic != "custom" && run.num("level", 0) > 0) {
      auto q = query(item, semantic == "model" ? "llm" : "actions", id, activity_query_state(request, semantic, config, item["binding"]));
      if(semantic == "action") {
        auto* action = q.add_actions(); action->set_action_id(id); action->set_operation(name);
        *action->mutable_arguments() = proto<google::protobuf::Struct>(prepared.object() ? prepared : Json{{"value", prepared}});
      }
      const auto decision = decide(item, q); item["query"] = json(q);
      if(semantic == "model") {
        const auto options = binding_options(item["binding"]);
        auto wrapped = proto<google::protobuf::Struct>(Json{{"payload", prepared}});
        wrapped = inject_request_context(wrapped, decision.context(), options);
        if(item.has("control_id")) {
          const auto& control = run["controls"][item.str("control_id")];
          if(control.str("actual_decision") == "replan")
            wrapped = inject_request_context(wrapped, control.str("context"), options);
        }
        prepared = json(wrapped)["payload"];
      } else { item["decision"] = decision.actions(0).decision(); reason = decision.actions(0).reason(); }
    }
    if(semantic != "custom") {
      fields = event_context(item); fields["action_id"] = id; fields["operation"] = name; fields["input"] = prepared;
      fields["attributes"] = Json{{"phase", "prepared"}, {"activity_kind", kind}, {"semantic", semantic}, {"decision", item["decision"]}};
      if(item.has("binding")) fields["attributes"]["request_binding"] = item["binding"];
      if(item.has("contract_check")) fields["attributes"]["contract_check"] = item["contract_check"];
      item["call_event"] = emit("action.start", fields, id, Json::Array{item["start_event"]});
    }
    run["activities"][id] = item;
    return Json{{"activity_id", id}, {"event_id", item["start_event"]}, {"decision", item["decision"]}, {"reason", reason},
      {"semantic", semantic}, {"input", prepared}, {"context", Json{{"run_id", run["identity"]["run_id"]}, {"activity_id", id}}}};
  }
  Json end_activity(const Json& request, bool interrupted = false) {
    keys(request, {"operation", "request_id", "run_id", "activity_id", "status", "output", "actual_input", "executed", "error", "extensions", "caused_by", "metrics"});
    const auto id = request.str("activity_id"), status = terminal(request);
    require(run["activities"].has(id), "activity_not_found", 404);
    auto& item = run["activities"][id]; require(!ended(item), "activity_already_ended", 409);
    require(request.has("output"), "activity_output_required");
    if(interrupted || status == "unknown") run["incomplete"] = true;
    const auto semantic = item.str("semantic");
    if(request.has("metrics")) {
      keys(request["metrics"], {"input_tokens", "output_tokens", "tokens", "cached_input_tokens"});
      for(const auto& metric : request["metrics"].fields())
        require(metric.second.integer() <= 1000000000, "metric_value_invalid");
      require(semantic == "model", "model_usage_required");
    }
    if(!interrupted && semantic != "custom") {
      require(request.has("executed"), "execution_observation_required");
      require(request["executed"].boolean() || status != "completed", "skipped_activity_cannot_succeed");
      if(semantic == "model") require(request.has("actual_input"), "actual_input_required");
    }
    auto fields = event_context(item); fields["output"] = request["output"]; fields["status"] = status;
    fields["attributes"] = Json{{"extensions", extensions(request)}};
    if(semantic == "model" && !interrupted && status == "completed" && request.flag("executed", false)) {
      const auto inferred = infer_model_usage(request["output"]);
      fields["attributes"]["metrics"] = inferred;
      // Explicit measured/mapped values take precedence over wire-format defaults.
      if(request.has("metrics")) for(const auto& metric : request["metrics"].fields())
        fields["attributes"]["metrics"][metric.first] = metric.second;
      const auto contracts = model_tool_contracts(request["actual_input"]);
      fields["attributes"]["tool_contracts"] = contracts;
      const auto scope = item.str("contract_scope");
      if(run["tool_contracts"].has(scope) || run["tool_contracts"].null() || run["tool_contracts"].fields().size() < 128) {
        auto& stored = run["tool_contracts"][scope];
        if(stored.null()) stored = contracts;
        else if(stored.object()) {
          for(auto& previous : stored.fields())
            if(!contracts.has(previous.first) || previous.second.dump() != contracts[previous.first].dump()) previous.second = nullptr;
          for(const auto& entry : contracts.fields())
            if(!stored.has(entry.first)) stored[entry.first] = entry.second;
          if(stored.fields().size() > 64 || stored.dump().size() > 65536) stored = "capacity_exceeded";
        }
      }
    }
    if(request.has("error")) {
      const auto error = proto<v1::ErrorInfo>(request["error"]);
      require(status != "completed" && identifier(error.code()) && (error.retryability() == "unknown"
          || error.retryability() == "retryable" || error.retryability() == "non_retryable"), "error_info_invalid");
      fields["error"] = request["error"];
    }
    Json links = causes(request);
    if(semantic != "custom") {
      fields["action_id"] = id;
      fields["attributes"]["semantic"] = semantic;
      if(request.has("metrics") && !interrupted && request.flag("executed", false))
        for(const auto& metric : request["metrics"].fields()) fields["attributes"]["metrics"][metric.first] = metric.second;
      if(request.has("actual_input")) fields["input"] = request["actual_input"];
      if(!interrupted) fields["attributes"]["executed"] = request["executed"];
      links.push(item["call_event"]);
      const auto call_end = emit("action.end", fields, id, join(links, id)); links = Json::Array{call_end};
      v1::RuntimeObservation observation;
      if(!interrupted) {
        observation.set_call_id(id); observation.set_transport_allowed(request["executed"].boolean());
        if(semantic == "model") *observation.mutable_request() = proto<google::protobuf::Struct>(Json{{"payload", request["actual_input"]}});
        else if(request["executed"].boolean()) observation.add_executed_action_ids(id);
        else observation.add_skipped_action_ids(id);
      }
      receipt(item, item["query"], observation, item["binding"]);
      if(semantic == "model" && item.has("control_id")) {
        auto& control = run["controls"][item.str("control_id")];
        if(control.str("decision") == "replan" && control.str("actual_decision") == "replan") {
          // Concurrent model calls have independent observations of the same replan.
          const auto actual = control.str("actual_decision") == "replan" ? observation : v1::RuntimeObservation{};
          receipt(control, control["query"], actual, item["binding"]); control["replan_observed"] = true;
        }
      }
      if(!interrupted) run["observed_boundaries"][semantic == "model" ? "model" : "actions"] = true;
    } else links.push(item["start_event"]);
    fields = event_context(item); fields["output"] = request["output"]; fields["status"] = status;
    fields["attributes"] = Json{{"extensions", extensions(request)}};
    item["end_event"] = emit("step.end", fields, item.str("step_id"), join(links, id));
    item["ended"] = true; item["status"] = status; item["output"] = request["output"];
    return Json{{"ended", true}, {"event_id", item["end_event"]}};
  }
  Json check_loop(const Json& request) {
    keys(request, {"operation", "request_id", "run_id", "loop_id", "state", "actor_id", "scope_id", "caused_by", "extensions", "apply_policy"});
    const bool apply_policy = request.flag("apply_policy", true);
    const auto loop_id = request.str("loop_id"), scope = scope_id(request, config);
    require(identifier(loop_id) && request.has("state"), "loop_fields_required");
    auto& loop = run["loops"][loop_id];
    if(loop.null()) loop = Json{{"scope_id", scope}, {"iteration", 0}, {"stopped", false}};
    require(loop.str("scope_id") == scope && !loop.flag("stopped", false), "loop_stopped_or_scope_changed", 409);
    Json links = causes(request);
    if(loop.has("control_id")) {
      const auto& previous = run["controls"][loop.str("control_id")];
      require(previous.flag("committed", false), "previous_control_unobserved", 409); links.push(previous["commit_event"]);
    }
    const auto id = "control-" + hash(request.str("request_id")).substr(0, 28);
    const auto iteration = loop.num("iteration", 0) + 1;
    Json item{{"step_id", "step-" + id}, {"stream_id", loop_id}, {"loop_id", loop_id}, {"iteration", std::to_string(iteration)},
      {"actor_id", request.str("actor_id", "unspecified")}, {"scope_id", scope}, {"decision", "continue"}, {"context", ""}, {"committed", false}};
    require(identifier(item.str("actor_id")), "activity_identity_invalid");
    auto fields = event_context(item); fields["input"] = request["state"];
    fields["attributes"] = Json{{"type", "boundary.control"}, {"policy_scope", scope}, {"apply_policy", apply_policy}, {"extensions", extensions(request)}};
    item["start_event"] = emit("step.start", fields, item.str("step_id"), join(links, id));
    if(apply_policy && run.num("level", 0) > 0) {
      auto q = query(item, "loop", id, request["state"]); q.set_iteration(iteration); q.set_state_fingerprint(hash(request["state"].dump()));
      if(loop.has("progress")) *q.mutable_previous() = proto<v1::LoopProgress>(loop["progress"]);
      bool pending = false;
      for(const auto& pair : run["activities"].fields()) pending |= !ended(pair.second) && pair.second.str("loop_id") == loop_id;
      q.set_pending_actions(pending);
      const auto decision = decide(item, q); item["query"] = json(q);
      item["decision"] = decision.progress().decision(); item["reason"] = decision.progress().reason(); item["context"] = decision.context();
      item["proposed_progress"] = json(decision.progress());
    }
    loop["iteration"] = iteration; loop["control_id"] = id; run["controls"][id] = item;
    return Json{{"control_id", id}, {"decision", item["decision"]}, {"reason", item.str("reason")},
      {"context", item["context"]}, {"iteration", iteration}, {"event_id", item["start_event"]}};
  }
  Json commit_loop(const Json& request, bool interrupted = false) {
    keys(request, {"operation", "request_id", "run_id", "control_id", "decision", "extensions", "caused_by"});
    const auto id = request.str("control_id"), decision = request.str("decision");
    require(run["controls"].has(id), "control_not_found", 404);
    require(decision == "continue" || decision == "replan" || decision == "stop", "loop_decision_invalid");
    auto& item = run["controls"][id]; require(!item.flag("committed", false), "control_already_observed", 409);
    auto fields = event_context(item); fields["decision"] = decision; fields["reason"] = "host_boundary_observation";
    fields["attributes"] = Json{{"extensions", extensions(request)}, {"interrupted", interrupted}};
    auto links = causes(request); links.push(item["start_event"]);
    if(!interrupted) item["commit_event"] = emit("loop.decision", fields, item.str("step_id"), join(links, id));
    if(item.str("decision") != "replan" || decision != "replan" || interrupted) {
      v1::RuntimeObservation observation; if(!interrupted) observation.set_loop_decision(decision);
      receipt(item, item["query"], observation); item["replan_observed"] = true;
    }
    fields = event_context(item); fields["status"] = interrupted ? "unknown" : "completed"; fields["output"] = nullptr;
    item["end_event"] = emit("step.end", fields, item.str("step_id"), Json::Array{interrupted ? item["start_event"] : item["commit_event"]});
    if(interrupted) item["commit_event"] = item["end_event"];
    item["committed"] = true; item["actual_decision"] = decision;
    auto& loop = run["loops"][item.str("loop_id")];
    if(item.has("proposed_progress")) {
      auto progress = proto<v1::LoopProgress>(item["proposed_progress"]);
      if(decision != item.str("decision") || interrupted) {
        const auto query = proto<v1::RuntimeQuery>(item["query"]);
        progress.set_replans(query.previous().replans());
        progress.set_no_progress(!query.state_fingerprint().empty() && query.previous().iteration() > 0
            && query.previous().fingerprint() == query.state_fingerprint() ? query.previous().no_progress() + 1 : 0);
        progress.clear_rule_id(); progress.clear_reason();
      }
      progress.set_decision(decision);
      loop["progress"] = json(progress);
    }
    if(decision == "stop") loop["stopped"] = true;
    if(!interrupted) run["observed_boundaries"]["loop"] = true;
    return Json{{"ended", true}, {"event_id", item["end_event"]}};
  }
};
}  // namespace

std::string bounded_query_text(const Json& value, const Json& config, bool task) {
  const auto available = config["shadow_options"].num("max_query_bytes", 32768);
  const auto budget = available > 2048 ? (available - 2048) / 2 : 128;
  return query_excerpt(value, std::min(budget, config["query_context"].num(task ? "max_task_bytes" : "max_state_bytes", 8192)));
}

Json boundary_metrics(const Json& events) {
  Json result{{"steps", 0}, {"llm_calls", 0}, {"tool_calls", 0}, {"loop_count", 0}, {"token_reported_calls", 0}};
  std::map<std::string, std::string> semantics;
  std::size_t input_calls = 0, output_calls = 0, total_calls = 0, inputs = 0, outputs = 0, totals = 0;
  for(const auto& event : events.items()) {
    const auto kind = event.str("kind");
    if(kind == "step.start") result["steps"] = result.num("steps", 0) + 1;
    if(kind == "loop.decision") result["loop_count"] = result.num("loop_count", 0) + 1;
    if(kind == "action.start") semantics[event.str("action_id")] = event["attributes"].str("semantic");
    if(kind != "action.end" || !event["attributes"].flag("executed", false)) continue;
    const auto semantic = semantics[event.str("action_id")];
    if(semantic == "action") result["tool_calls"] = result.num("tool_calls", 0) + 1;
    if(semantic != "model") continue;
    result["llm_calls"] = result.num("llm_calls", 0) + 1;
    const auto& metrics = event["attributes"]["metrics"];
    if(metrics.has("input_tokens")) { ++input_calls; inputs += metrics.num("input_tokens", 0); }
    if(metrics.has("output_tokens")) { ++output_calls; outputs += metrics.num("output_tokens", 0); }
    if(metrics.has("tokens") || (metrics.has("input_tokens") && metrics.has("output_tokens"))) {
      ++total_calls; totals += metrics.has("tokens") ? metrics.num("tokens", 0)
          : metrics.num("input_tokens", 0) + metrics.num("output_tokens", 0);
    }
  }
  // Unknown usage stays unknown, rather than making an expensive trial look free.
  const auto calls = result.num("llm_calls", 0);
  if(input_calls == calls) result["input_tokens"] = inputs;
  if(output_calls == calls) result["output_tokens"] = outputs;
  if(total_calls == calls) result["tokens"] = totals;
  result["token_reported_calls"] = total_calls;
  return result;
}

Json boundary_trace(const Json& run) {
  Json attributes{{"capture_complete", !run.flag("incomplete", false)}};
  if(run.has("local_degradation_reason")) attributes["degradation_reason"] = run["local_degradation_reason"];
  if(run.flag("local_capture", false)) attributes["transport"] = "local.native.v1";
  if(run.has("profile")) attributes["protocol"] = run["profile"];
  if(run.has("extensions")) attributes["extensions"] = run["extensions"];
  if(run.num("metrics_version", 0) == 1) attributes["metrics"] = boundary_metrics(run["events"]);
  return Json{{"schema_version", 1}, {"trace_kind", "trajectory.v1"}, {"identity", run["identity"]},
    {"events", run["events"]}, {"attributes", attributes}};
}

void validate_boundary_configuration(const Json& config) {
  if(config.has("query_context")) {
    keys(config["query_context"], {"max_task_bytes", "max_state_bytes"});
    for(const auto* key : {"max_task_bytes", "max_state_bytes"})
      require(config["query_context"].num(key, 8192) >= 128 && config["query_context"].num(key, 8192) <= 32768,
          "query_context_limits_invalid");
  }
  if(!config.has("activity_types")) return;
  require(config["activity_types"].object() && config["activity_types"].fields().size() <= 128, "activity_types_invalid");
  for(const auto& pair : config["activity_types"].fields()) {
    require(identifier(pair.first) && pair.first != "model" && pair.first != "action" && pair.first != "custom", "activity_type_invalid");
    keys(pair.second, {"semantic", "required_input_paths"});
    const auto semantic = pair.second.str("semantic");
    require(semantic == "model" || semantic == "action" || semantic == "custom", "activity_semantic_invalid");
    if(pair.second.has("required_input_paths")) {
      require(pair.second["required_input_paths"].items().size() <= 64, "too_many_required_input_fields");
      for(const auto& path : pair.second["required_input_paths"].items())
        require(path.text().size() <= 256 && rapidjson::Pointer(path.text().c_str()).IsValid(), "input_pointer_invalid");
    }
  }
}
Json describe_boundary_contract(const Json& config) {
  Json result{{"protocol", "evolution.boundary.v1"}, {"contract_version", 1}, {"max_level", config["max_level"]},
    {"operations", Json::Array{"point", "run.begin", "run.heartbeat", "activity.begin", "activity.end", "activity.abort", "activity.status", "loop.check", "loop.commit", "loop.abort",
      "event.record", "capture.gap", "run.end", "run.abort", "run.status", "evaluation.retry", "memory.status"}},
    {"required_fields", Json{{"run.begin", Json::Array{"input"}}, {"activity.begin", Json::Array{"kind", "name", "input"}},
      {"activity.end", Json::Array{"activity_id", "status", "output"}}, {"loop.check", Json::Array{"loop_id", "state"}},
      {"loop.commit", Json::Array{"control_id", "decision"}}, {"event.record", Json::Array{"type", "data"}}}},
    {"optional_fields", Json::Array{"actor_id", "scope_id", "stream_id", "control_id", "caused_by", "extensions"}},
    {"extension_field", "extensions"}, {"policy_scopes", config["actors"]},
    {"activity_types", config.has("activity_types") ? config["activity_types"] : Json(Json::Object{})},
    {"limits", Json{{"max_request_bytes", config.num("max_request_bytes", 1048576)}, {"max_events", config.num("max_events", 10000)}}}};
  if(config.has("policy_evaluation_file"))
    for(const auto* operation : {"agent_evaluation.status", "agent_evaluation.feedback", "agent_evaluation.submit", "agent_evaluation.retry", "agent_evaluation.cancel", "agent_evaluation.tree"})
      result["operations"].push(operation);
  return result;
}
Json boundary_begin_request(const Json& request, const std::string& run_id) {
  keys(request, {"operation", "request_id", "contract_version", "input", "conversation_id", "level", "capabilities", "extensions"});
  require(request.num("contract_version", 1) == 1, "contract_version_not_supported");
  require(request.has("input"), "run_input_required"); extensions(request);
  return Json{{"operation", "task.begin"}, {"request_id", request["request_id"]}, {"input", request["input"]},
    {"conversation_id", request.str("conversation_id", run_id)}, {"level", request.str("level", "L0")},
    {"capabilities", request.has("capabilities") ? request["capabilities"] : Json(Json::Array{})}};
}
void initialize_boundary_run(Json& run, const Json& request) {
  run["profile"] = "evolution.boundary.v1";
  run["metrics_version"] = 1;
  run["activities"] = Json::Object{}; run["loops"] = Json::Object{}; run["controls"] = Json::Object{};
  run["extensions"] = extensions(request);
  run["events"].items().front()["attributes"] = Json{{"protocol", "evolution.boundary.v1"}, {"extensions", run["extensions"]}};
  if(run.has("agent_version_id")) run["events"].items().front()["attributes"]["agent_version_id"] = run["agent_version_id"];
  if(run.has("evaluation_trial_id")) run["events"].items().front()["attributes"]["evaluation_trial_id"] = run["evaluation_trial_id"];
}
Json dispatch_boundary(Json& run, const Json& request, const Json& config, const PolicyRuntime* cached_policy) {
  Runtime runtime(run, config, cached_policy); const auto op = request.str("operation");
  if(op == "activity.status") {
    keys(request, {"operation", "request_id", "run_id", "activity_id"});
    require(run["activities"].has(request.str("activity_id")), "activity_not_found", 404);
    const auto& item = run["activities"][request.str("activity_id")];
    return Json{{"activity_id", item["activity_id"]}, {"ended", ended(item)}, {"status", item.str("status", "pending")},
      {"output", item["output"]}, {"event_id", item.str("end_event")}};
  }
  require(!run.flag("closed", false), "task_already_closed", 409);
  if(op == "run.heartbeat") {
    keys(request, {"operation", "request_id", "run_id"});
    return Json{{"alive", true}};
  }
  if(op == "activity.begin") return runtime.begin_activity(request);
  if(op == "activity.end" || op == "activity.abort") {
    if(op == "activity.abort") require(terminal(request) != "completed", "abort_status_invalid");
    return runtime.end_activity(request, op == "activity.abort");
  }
  if(op == "loop.check") return runtime.check_loop(request);
  if(op == "loop.commit" || op == "loop.abort") {
    if(op == "loop.abort") run["incomplete"] = true;
    return runtime.commit_loop(request, op == "loop.abort");
  }
  if(op == "event.record") {
    keys(request, {"operation", "request_id", "run_id", "type", "data", "activity_id", "control_id", "caused_by", "extensions"});
    require(identifier(request.str("type")) && request.has("data"), "event_fields_required");
    require(!request.has("activity_id") || !request.has("control_id"), "event_context_exclusive");
    auto fields = Json{{"attributes", Json{{"type", "extension.event"}, {"event_type", request["type"]},
      {"data", request["data"]}, {"extensions", extensions(request)}}}};
    if(request.has("activity_id")) {
      require(run["activities"].has(request.str("activity_id")), "activity_not_found", 404);
      const auto context = event_context(run["activities"][request.str("activity_id")]);
      for(const auto& pair : context.fields()) fields[pair.first] = pair.second;
    } else if(request.has("control_id")) {
      // A point may report a local result after loop.commit, without opening
      // another activity. Keep its explicit round, not the latest sibling loop.
      require(run["controls"].has(request.str("control_id")), "control_not_found", 404);
      const auto context = event_context(run["controls"][request.str("control_id")]);
      for(const auto& pair : context.fields()) fields[pair.first] = pair.second;
    }
    return Json{{"event_id", runtime.emit("custom", fields, "extension-" + hash(request.str("request_id")).substr(0, 24), runtime.causes(request))}};
  }
  if(op == "run.end" || op == "run.abort") {
    keys(request, {"operation", "request_id", "run_id", "status", "business_status", "output", "capture_complete", "extensions", "defer_if_pending", "caused_by"});
    const auto status = terminal(request), business = request.str("business_status", "unknown");
    require(request.has("output"), "run_output_required");
    require(business == "succeeded" || business == "failed" || business == "partial" || business == "unknown", "business_status_invalid");
    const bool interrupted = op == "run.abort";
    if(!interrupted && request.flag("defer_if_pending", false)) {
      bool pending = false;
      for(const auto& pair : run["activities"].fields()) pending |= !ended(pair.second);
      for(const auto& pair : run["controls"].fields()) pending |= !pair.second.flag("committed", false);
      if(pending) return Json{{"closed", false}, {"pending", true}, {"capture_complete", !run.flag("incomplete", false)}};
    }
    require(!interrupted || status != "completed", "abort_status_invalid");
    if(interrupted || status == "unknown" || !request.flag("capture_complete", true)) run["incomplete"] = true;
    for(auto& pair : run["activities"].fields()) if(!ended(pair.second)) {
      require(interrupted, "run_has_pending_activities", 409);
      runtime.end_activity(Json{{"activity_id", pair.first}, {"status", "unknown"}, {"output", nullptr}}, true);
    }
    for(auto& pair : run["controls"].fields()) {
      auto& control = pair.second;
      if(!control.flag("committed", false)) {
        require(interrupted, "run_has_unobserved_controls", 409);
        runtime.commit_loop(Json{{"control_id", pair.first}, {"decision", "stop"}}, true);
      }
      if(control.str("decision") == "replan" && !control.flag("replan_observed", false)) {
        runtime.receipt(control, control["query"], v1::RuntimeObservation{}); control["replan_observed"] = true;
      }
    }
    runtime.emit("run.end", Json{{"output", request["output"]}, {"outcome", Json{{"execution_status", status}, {"business_status", business}}},
      {"attributes", Json{{"extensions", extensions(request)}}}}, "task", runtime.causes(request));
    run["closed"] = true;
    run["evaluation_status"] = run.flag("incomplete", false) ? "quarantined" : (config.flag("auto_evaluate", true) ? "pending" : "disabled");
    return Json{{"closed", true}, {"capture_complete", !run.flag("incomplete", false)},
      {"evaluation_status", run["evaluation_status"]}, {"metrics", boundary_metrics(run["events"])}};
  }
  throw Error("operation_not_found", 404);
}
}  // namespace trajectory::evolution::integration
