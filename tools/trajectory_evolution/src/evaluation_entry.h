#pragma once
#include "integration_json.h"
#include <rapidjson/pointer.h>

namespace trajectory::evolution::integration {

// Trial-local delivery; dispatch acknowledgements never close a run. The
// IntegrationService mutex owns this state. No framework code is called here.
struct EvaluationEntry {
  Json specification, packet, messages = Json::Array{};
  std::string consumer, run_id, failure;
  std::size_t index = 0;
  bool delivered = false, done = false;

  static Json render(const Json& pattern, const Json& values, std::size_t depth = 0) {
    require(depth <= 32, "evaluation_entry_template_too_deep");
    if(pattern.object()) {
      if(pattern.has("$eval")) {
        require(pattern.fields().size() == 1 && values.has(pattern.str("$eval")), "evaluation_entry_binding_invalid");
        return values[pattern.str("$eval")];
      }
      Json out = Json::Object{};
      for(const auto& p : pattern.fields()) out[p.first] = render(p.second, values, depth + 1);
      return out;
    }
    if(pattern.array()) {
      Json out = Json::Array{};
      for(const auto& item : pattern.items()) out.push(render(item, values, depth + 1));
      return out;
    }
    return pattern;
  }
  static Json fields() {
    return Json{{"input", nullptr}, {"messages", nullptr}, {"user_id", nullptr},
      {"conversation_id", nullptr}, {"run_key", nullptr}, {"turn", nullptr},
      {"initial_state", nullptr}, {"constraints", nullptr}, {"budget", nullptr}};
  }
  static void validate(const Json& config) {
    keys(config, {"input", "output_pointer"});
    require(config.has("input"), "evaluation_entry_input_required");
    render(config["input"], fields());
    require(uses(config["input"], "input") || uses(config["input"], "messages"), "evaluation_entry_task_unbound");
    const auto path = config.str("output_pointer");
    require(path.size() <= 2048 && path.find('\0') == std::string::npos
        && rapidjson::Pointer(path.c_str()).IsValid(), "evaluation_entry_output_pointer_invalid");
  }
  static bool uses(const Json& pattern, const std::string& name) {
    if(pattern.object()) {
      if(pattern.has("$eval")) return pattern.str("$eval") == name;
      for(const auto& p : pattern.fields()) if(uses(p.second, name)) return true;
    } else if(pattern.array()) for(const auto& p : pattern.items()) if(uses(p, name)) return true;
    return false;
  }
  bool enabled() const { return !specification.null(); }
  std::size_t count() const { return specification["task"]["followup_inputs"].items().size() + 1; }
  void before_run() {
    if(enabled() && !(delivered && !done && failure.empty() && run_id.empty())) {
      failure = "evaluation_entry_unexpected_run";
      throw Error("evaluation_entry_unexpected_run");
    }
  }
  void advance(const Json& run) {
    if(run.flag("incomplete", false) || run["events"].items().empty()
        || run["events"].items().back().str("kind") != "run.end") {
      failure = "evaluation_entry_trace_incomplete"; return;
    }
    const auto& output = run["events"].items().back()["output"];
    rapidjson::Document document;
    const auto bytes = output.dump(); document.Parse(bytes.c_str());
    const auto* value = rapidjson::Pointer(specification["binding"].str("output_pointer").c_str()).Get(document);
    if(!value) { failure = "evaluation_entry_output_missing"; return; }
    auto content = Json::convert(*value);
    messages.push(Json{{"role", "assistant"}, {"content", content.string() ? content : Json(content.dump_for_transport())}});
    if(messages.dump().size() > 1048576) { failure = "evaluation_entry_history_limit"; return; }
    ++index; delivered = false; run_id.clear(); packet = nullptr;
    done = index == count();
  }
  Json next(const Json& request, const Json& run, bool persisted) {
    keys(request, {"operation", "request_id", "consumer", "index"});
    require(enabled(), "evaluation_entry_not_enabled", 409);
    const auto who = request.str("consumer");
    require(!who.empty() && who.size() <= 128 && who.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos,
        "evaluation_entry_consumer_invalid");
    require(consumer.empty() || consumer == who, "evaluation_entry_already_bound", 409);
    const auto requested = request["index"].integer();
    require(requested == index || (delivered && requested == index + 1), "evaluation_entry_sequence_invalid", 409);
    consumer = who;
    if(!failure.empty()) return Json{{"state", "failed"}};
    if(requested == index + 1) {
      if(run.null() || !run.flag("closed", false) || !persisted)
        return Json{{"state", "waiting"}, {"retry_after_ms", 100}};
      advance(run);
    }
    if(!failure.empty()) return Json{{"state", "failed"}};
    if(done) return Json{{"state", "done"}};
    if(!delivered) {
      const auto& task = specification["task"];
      auto values = fields();
      values["input"] = index ? task["followup_inputs"].items().at(index - 1) : task["task_input"];
      messages.push(Json{{"role", "user"}, {"content", values["input"]}});
      values["messages"] = messages;
      values["user_id"] = specification["user_id"];
      values["conversation_id"] = specification["trial_id"];
      values["run_key"] = specification.str("trial_id") + "-" + std::to_string(index);
      values["turn"] = index;
      for(const auto* name : {"initial_state", "constraints", "budget"}) values[name] = task[name];
      packet = Json{{"state", "input"}, {"index", index}, {"input", render(specification["binding"]["input"], values)}};
      require(packet.dump().size() <= 1048576, "evaluation_entry_input_limit");
      delivered = true;
    }
    return packet;
  }
};

}  // namespace trajectory::evolution::integration
