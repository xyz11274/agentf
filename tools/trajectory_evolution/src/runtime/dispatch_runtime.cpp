#include "dispatch_runtime.h"

#include <rapidjson/pointer.h>
#include <openssl/sha.h>
#include <set>

namespace trajectory::evolution::integration {
namespace {
std::string fingerprint(const Json& value) {
  const auto bytes = value.dump(); unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
  std::string result; const char* hex = "0123456789abcdef";
  for(auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
Json read_path(const Json& value, const std::string& path) {
  rapidjson::Document doc; const auto raw = value.dump(); doc.Parse(raw.data(), raw.size());
  const auto* found = rapidjson::Pointer(path.c_str()).Get(doc);
  return found ? Json::convert(*found) : Json{};
}
void write_path(Json& value, const std::string& path, const Json& replacement) {
  rapidjson::Document doc, content;
  const auto raw = value.dump(), bytes = replacement.dump();
  doc.Parse(raw.data(), raw.size()); content.Parse(bytes.data(), bytes.size());
  rapidjson::Pointer(path.c_str()).Set(doc, content, doc.GetAllocator()); value = Json::convert(doc);
}
void binding_valid(const Json& binding) {
  keys(binding, {"items", "operation", "receipt", "accepted", "accepted_index", "accepted_id",
      "results", "result_id", "result_success", "result_operation", "result_status", "result_output", "action_id", "after"});
  for(const auto* field : {"items", "operation", "receipt", "accepted", "accepted_index", "accepted_id",
      "results", "result_id", "result_success", "result_operation", "result_status", "result_output"}) {
    const auto path = binding.str(field);
    require(!path.empty() && path.size() <= 256 && path.front() == '/' && path.find('\0') == std::string::npos
        && rapidjson::Pointer(path.c_str()).IsValid(), "dispatch_binding_invalid");
  }
  require(binding.has("action_id") == binding.has("after"), "dispatch_dependency_binding_incomplete");
  for(const auto* field : {"action_id", "after"}) if(binding.has(field)) {
    const auto path = binding.str(field);
    require(!path.empty() && path.size() <= 256 && path.front() == '/' && path.find('\0') == std::string::npos
        && rapidjson::Pointer(path.c_str()).IsValid(), "dispatch_binding_invalid");
  }
}
Json subpoint(Json& run, const Json& state, const Json& data, const Json& config,
              const PolicyRuntime* policy, const std::string& suffix) {
  Json request = data; request["operation"] = "point"; request["kind"] = "schedule";
  request["ref"] = Json{{"run", state["ref"]["run"]}, {"parent", state["ref"]["key"]},
      {"key", state.str("id") + ":" + suffix}};
  return dispatch_referenced_point(run, request, config, policy);
}
Json observe(Json& run, Json& state, const std::string& action, const std::string& status,
             const Json& output, const Json& config, const PolicyRuntime* policy) {
  return subpoint(run, state, Json{{"phase", "event"}, {"input", Json{{"action_id", action},
      {"status", status}, {"output", output}}}}, config, policy, action + ":" + status);
}
Json missing_result(const Json& state, const std::string& action, const std::string& status) {
  const auto& b = state["binding"];
  const auto entry = std::find_if(state["actions"].items().begin(), state["actions"].items().end(),
      [&](const auto& item) { return item.str("action_id") == action; });
  require(entry != state["actions"].items().end(), "dispatch_action_not_found");
  Json result = Json::Object{};
  write_path(result, b.str("result_id"), state.str("id") + ":" + action);
  write_path(result, b.str("result_operation"), (*entry)["operation"]);
  write_path(result, b.str("result_success"), false);
  write_path(result, b.str("result_status"), status);
  write_path(result, b.str("result_output"), Json{{"status", status}});
  if(status != "unknown") result["executed"] = false;
  return result;
}
Json next_wave(Json& run, Json& state, const Json& config, const PolicyRuntime* policy) {
  auto view = subpoint(run, state, Json{{"phase", "event"}, {"input", Json::Object{}}}, config, policy,
      "view:" + std::to_string(state.num("sequence", 0)));
  for(const auto& skipped : view["skipped"].items()) if(!state["outcomes"].has(skipped.text())) {
    auto result = missing_result(state, skipped.text(), "skipped");
    observe(run, state, skipped.text(), "skipped", result, config, policy);
    state["outcomes"][skipped.text()] = result;
  }
  if(view.flag("finished", false)) {
    Json completion = state.has("completion") ? state["completion"] : Json(Json::Object{});
    Json results = Json::Array{};
    for(const auto& action : state["actions"].items()) results.push(state["outcomes"][action.str("action_id")]);
    write_path(completion, state["binding"].str("receipt"), state["id"]);
    write_path(completion, state["binding"].str("results"), results);
    dispatch_referenced_point(run, Json{{"operation", "point"}, {"phase", "after"}, {"kind", "schedule"},
        {"ref", state["ref"]}, {"output", completion}}, config, policy);
    state["finished"] = true; state["completion"] = completion;
    return Json{{"managed", true}, {"pending", false}, {"completion", completion}};
  }
  require(!view["ready"].items().empty(), "dispatch_completion_pending", 409);
  const auto seq = state.num("sequence", 0) + 1; state["sequence"] = seq;
  Json wave{{"id", state.str("id") + ":wave:" + std::to_string(seq)}, {"claimed", false},
      {"accepted", false}, {"actions", view["ready"]}};
  Json items = Json::Array{};
  for(const auto& action : view["ready"].items()) items.push(action["arguments"]);
  auto payload = state["payload"]; write_path(payload, state["binding"].str("items"), items);
  wave["input"] = payload; state["wave"] = wave;
  return Json{{"managed", true}, {"pending", false}, {"wave_id", wave["id"]}, {"input", payload},
      {"plan_ref", state["ref"]}};
}
}

Json dispatch_batch_point(Json& run, const Json& request, const Json& config, const PolicyRuntime* policy) {
  keys(request, {"operation", "request_id", "kind", "phase", "ref", "name", "input", "binding", "actor_id",
      "scope_id", "extensions", "apply_policy", "capture_gap"});
  point_run_key(request);
  const auto phase = request.str("phase"), command = request.str("name");
  if(phase == "before") {
    binding_valid(request["binding"]);
    const auto items = read_path(request["input"], request["binding"].str("items"));
    require(items.array() && !items.items().empty() && items.items().size() <= 128, "dispatch_items_invalid");
    Json actions = Json::Array{};
    for(std::size_t i = 0; i < items.items().size(); ++i) {
      const auto& item = items.items()[i];
      const auto operation = read_path(item, request["binding"].str("operation"));
      require(item.object() && operation.string() && identifier(operation.text()), "dispatch_operation_invalid");
      Json action{{"action_id", std::to_string(i + 1)}, {"operation", operation}, {"arguments", item}};
      if(request["binding"].has("action_id")) {
        action["action_id"] = read_path(item, request["binding"].str("action_id"));
        const auto after = read_path(item, request["binding"].str("after"));
        require(after.array(), "dispatch_dependencies_invalid"); action["after"] = after;
      }
      actions.push(action);
    }
    auto before = request; before["kind"] = "schedule"; before.fields().erase("binding");
    before["input"] = Json{{"actions", actions}};
    const auto prepared = dispatch_referenced_point(run, before, config, policy);
    const auto key = point_call_key(request);
    Json state{{"id", prepared["context"]["activity_id"]}, {"ref", request["ref"]}, {"binding", request["binding"]},
        {"payload", request["input"]}, {"actions", actions}, {"outcomes", Json::Object{}}, {"finished", false}};
    auto result = next_wave(run, state, config, policy); run["dispatch_plans"][key] = state;
    return result;
  }
  require(phase == "event", "dispatch_phase_invalid");
  if(command == "complete") {
    require(request["input"].object(), "dispatch_completion_invalid");
    // Physical receipts are scoped to this authenticated run. No framework,
    // process ID or user-supplied callback implementation enters the component.
    std::string selected;
    if(run["dispatch_plans"].object()) for(const auto& entry : run["dispatch_plans"].fields()) {
      const auto& state = entry.second;
      const auto receipt = read_path(request["input"], state["binding"].str("receipt"));
      if(receipt.string() && state["receipts"].has(receipt.text())) {
        require(selected.empty(), "dispatch_receipt_ambiguous", 409); selected = entry.first;
      }
    }
    if(selected.empty()) return Json{{"managed", false}, {"pending", false}, {"completion", request["input"]}};
    auto& state = run["dispatch_plans"][selected]; const auto& b = state["binding"];
    const auto receipt = read_path(request["input"], b.str("receipt")).text();
    if(state["receipts"][receipt].flag("completed", false)) {
      require(state["receipts"][receipt].str("fingerprint") == fingerprint(request["input"]),
          "dispatch_completion_changed", 409);
      return state["receipts"][receipt]["response"];
    }
    const auto results = read_path(request["input"], b.str("results"));
    require(results.array(), "dispatch_results_invalid");
    auto expected = state["receipts"][receipt]["actions"];
    std::set<std::string> seen;
    for(const auto& result : results.items()) {
      const auto id = read_path(result, b.str("result_id"));
      require(result.object() && id.string() && expected.has(id.text()) && seen.insert(id.text()).second,
          "dispatch_result_identity_invalid");
      const auto action = expected[id.text()].text();
      const auto success = read_path(result, b.str("result_success"));
      const auto status = std::holds_alternative<bool>(success.value) ? (success.boolean() ? "succeeded" : "failed") : "unknown";
      observe(run, state, action, status, result, config, policy); state["outcomes"][action] = result;
    }
    for(const auto& entry : expected.fields()) if(!seen.count(entry.first)) {
      auto result = missing_result(state, entry.second.text(), "unknown");
      observe(run, state, entry.second.text(), "unknown", result, config, policy);
      state["outcomes"][entry.second.text()] = result; run["incomplete"] = true;
    }
    state["completion"] = request["input"]; state["receipts"][receipt]["completed"] = true;
    const auto response = next_wave(run, state, config, policy);
    state["receipts"][receipt]["fingerprint"] = fingerprint(request["input"]);
    state["receipts"][receipt]["response"] = response;
    return response;
  }
  require(command == "claim" || command == "accept" || command == "abort", "dispatch_command_invalid");
  auto ref = request; ref["ref"]["key"] = request["ref"]["parent"];
  const auto key = point_call_key(ref);
  require(run["dispatch_plans"].has(key), "dispatch_plan_not_found", 404);
  auto& state = run["dispatch_plans"][key]; auto& wave = state["wave"];
  if(command == "claim") {
    if(state.flag("finished", false)) return Json{{"execute", false}, {"pending", false},
        {"completion", state["completion"]}, {"output", state["ack"]}};
    if(request["input"].str("wave_id") != wave.str("id"))
      return Json{{"execute", false}, {"pending", true}, {"output", state["ack"]}};
    if(wave.flag("claimed", false)) return Json{{"execute", false}, {"pending", true}, {"output", state["ack"]}};
    for(const auto& action : wave["actions"].items()) observe(run, state, action.str("action_id"), "running", nullptr, config, policy);
    wave["claimed"] = true;
    return Json{{"execute", true}, {"input", wave["input"]}};
  }
  require(!state.flag("finished", false) && request["input"].str("wave_id") == wave.str("id"), "dispatch_wave_invalid", 409);
  require(wave.flag("claimed", false) && !wave.flag("accepted", false), "dispatch_wave_not_claimed", 409);
  if(command == "abort") {
    for(const auto& action : wave["actions"].items()) observe(run, state, action.str("action_id"), "unknown", nullptr, config, policy);
    run["incomplete"] = true;
    dispatch_referenced_point(run, Json{{"operation", "point"}, {"phase", "abort"}, {"kind", "schedule"},
        {"ref", state["ref"]}, {"output", nullptr}}, config, policy);
    state["finished"] = true; return Json{{"managed", true}, {"pending", false}};
  }
  const auto& b = state["binding"]; const auto& ack = request["input"]["output"];
  require(ack.object(), "dispatch_ack_invalid");
  const auto accepted = read_path(ack, b.str("accepted"));
  require(accepted.array(), "dispatch_accepted_invalid");
  const auto receipt = read_path(ack, b.str("receipt"));
  Json identities = Json::Object{}; std::set<std::size_t> indices;
  for(const auto& item : accepted.items()) {
    const auto index = read_path(item, b.str("accepted_index")).integer();
    const auto id = read_path(item, b.str("accepted_id"));
    require(index >= 1 && index <= wave["actions"].items().size() && indices.insert(index).second
        && id.string() && identifier(id.text()) && !identities.has(id.text()), "dispatch_accepted_identity_invalid");
    identities[id.text()] = wave["actions"].items()[index - 1]["action_id"];
  }
  for(std::size_t i = 0; i < wave["actions"].items().size(); ++i) if(!indices.count(i + 1)) {
    const auto action = wave["actions"].items()[i].str("action_id");
    auto result = missing_result(state, action, "dispatch_failed");
    observe(run, state, action, "failed", result, config, policy); state["outcomes"][action] = result;
  }
  wave["accepted"] = true;
  if(identities.fields().empty()) return next_wave(run, state, config, policy);
  require(receipt.string() && identifier(receipt.text()) && !state["receipts"].has(receipt.text()), "dispatch_receipt_invalid");
  state["receipts"][receipt.text()] = Json{{"actions", identities}, {"completed", false}};
  auto output = ack; write_path(output, b.str("receipt"), state["id"]); state["ack"] = output;
  return Json{{"managed", true}, {"pending", true}, {"output", output}};
}

}  // namespace trajectory::evolution::integration
