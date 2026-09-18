// GEPA 搜索负责候选生成、评测调度与节点选择；宿主执行交给统一评测器。
#include "gepa_search.h"
#include "gepa_strategy.h"
#include "control_generation.h"
#include "failure_ledger.h"
#include "evaluation_contracts.h"
#include "local_support.h"
#include "model_json.h"
#include "agentflow/trajectory/runtime_config.h"
#include "trajectory_evolution/agent_evaluation.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <openssl/sha.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace trajectory::evolution::integration {
namespace {
namespace fs = std::filesystem;
namespace legacy = agentflow::trajectory;
using Report = dataset::v1::TaskEvaluationReport;

std::string hash(const std::string& bytes) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), out), "gepa_hash_failed");
  std::string result;
  for(auto c : out) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
  return result;
}
std::size_t now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void check_path(const fs::path& path) {
  fs::path prefix;
  for(const auto& part : fs::absolute(path)) { prefix /= part; require(!fs::is_symlink(prefix), "gepa_symlink_rejected"); }
}
std::string read(const fs::path& path, std::size_t limit = 4194304) {
  check_path(path);
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(stream && stream.tellg() >= 0 && static_cast<std::size_t>(stream.tellg()) <= limit, "gepa_file_unavailable");
  std::string bytes(static_cast<std::size_t>(stream.tellg()), '\0'); stream.seekg(0);
  require(bytes.empty() || bool(stream.read(bytes.data(), bytes.size())), "gepa_read_failed"); return bytes;
}
void write(const fs::path& path, const std::string& bytes) {
  check_path(path); fs::create_directories(path.parent_path());
  require(::chmod(path.parent_path().c_str(), 0700) == 0, "gepa_permission_failed");
  std::string pending = path.string() + ".pending.XXXXXX";
  int fd = ::mkstemp(pending.data()); require(fd >= 0, "gepa_write_failed");
  try {
    for(std::size_t offset = 0; offset < bytes.size();) {
      const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset); require(n > 0, "gepa_write_failed"); offset += n;
    }
    require(::fsync(fd) == 0, "gepa_write_failed"); ::close(fd); fd = -1;
    require(::rename(pending.c_str(), path.c_str()) == 0, "gepa_write_failed");
    int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if(parent >= 0) { ::fsync(parent); ::close(parent); }
  } catch(...) { if(fd >= 0) ::close(fd); ::unlink(pending.c_str()); throw; }
}
double number(const Json& value, double fallback = 0) {
  return std::holds_alternative<double>(value.value) ? std::get<double>(value.value) : fallback;
}
std::size_t bound(const Json& c, const char* key, std::size_t fallback, std::size_t low, std::size_t high) {
  const auto n = c.num(key, fallback); require(n >= low && n <= high, "gepa_limit_invalid"); return n;
}
bool scored(const Report& report) {
  return report.complete() && report.trials_size() && std::all_of(report.trials().begin(), report.trials().end(), [](const auto& t) {
    return t.verdict() == "passed" || t.verdict() == "failed";
  });
}

// Preserve concrete text while bounding each field and the complete packet.
std::string clip(std::string value, std::size_t limit) {
  if(value.size() <= limit) return value;
  while(limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
  value.resize(limit); return value + " [truncated]";
}
Json excerpt(const Json& value, unsigned depth = 0) {
  if(value.string()) return clip(value.text(), 2048);
  if(depth >= 12) return "[depth limit]";
  if(value.array()) {
    Json result = Json::Array{};
    for(const auto& item : value.items()) { if(result.items().size() == 24) { result.push("[items truncated]"); break; } result.push(excerpt(item, depth + 1)); }
    return result;
  }
  if(value.object()) {
    Json result = Json::Object{};
    for(const auto& item : value.fields()) { if(result.fields().size() == 32) { result["_truncated"] = true; break; } result[item.first] = excerpt(item.second, depth + 1); }
    return result;
  }
  return value;
}
Json compact_excerpt(const Json& value, unsigned depth = 0) {
  if(value.string()) return clip(value.text(), 512);
  if(depth >= 8) return "[depth limit]";
  if(value.array()) {
    Json result = Json::Array{};
    for(const auto& item : value.items()) {
      if(result.items().size() == 8) { result.push("[items truncated]"); break; }
      result.push(compact_excerpt(item, depth + 1));
    }
    return result;
  }
  if(value.object()) {
    Json result = Json::Object{};
    for(const auto& item : value.fields()) {
      if(result.fields().size() == 16) { result["_truncated"] = true; break; }
      result[item.first] = compact_excerpt(item.second, depth + 1);
    }
    return result;
  }
  return value;
}
Json parse_model_object(const std::string& response, bool* recovered = nullptr) {
  if(recovered) *recovered = false;
  const auto body = legacy::unwrap_model_json(response);
  try {
    auto value = Json::parse(body);
    require(value.object(), "invalid_json");
    return value;
  } catch(const std::exception&) {
    // Some OpenAI-compatible providers add a short explanation around an
    // otherwise valid JSON object despite response_format=json_object. Accept
    // exactly one balanced, parseable object; multiple objects stay ambiguous.
  }
  Json only;
  std::size_t count = 0, start = 0, depth = 0;
  bool quoted = false, escaped = false;
  for(std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if(depth && quoted) {
      if(escaped) escaped = false;
      else if(c == '\\') escaped = true;
      else if(c == '"') quoted = false;
      continue;
    }
    if(depth && c == '"') { quoted = true; continue; }
    if(c == '{') {
      if(!depth) start = i;
      ++depth;
    } else if(c == '}' && depth && !--depth) {
      try {
        auto candidate = Json::parse(body.substr(start, i - start + 1));
        if(candidate.object()) { only = std::move(candidate); ++count; }
      } catch(const std::exception&) {}
    }
  }
  require(count == 1, "invalid_json");
  if(recovered) *recovered = true;
  return only;
}
Json all_source_experiences(const Json& artifact) {
  Json result = Json::Array{};
  const auto policy = proto<v1::RuntimePolicy>(artifact["policy"]);
  for(const auto& item : policy.snapshot().experiences()) {
    if(!item.control_target_type().empty()) continue;
    bool approved = false;
    for(const auto& ref : policy.options().approved_experiences())
      approved |= ref.memory_id() == item.memory_id() && ref.content_revision() == item.content_revision();
    if(!approved) continue;
    Json value{{"memory_id", item.memory_id()}, {"scope", item.target_scope()},
        {"condition", item.trigger_pattern()}, {"content", item.content()}};
    for(const auto& rewrite : policy.options().experience_overrides())
      if(rewrite.source_experience().memory_id() == item.memory_id()) {
        value["content"] = rewrite.content();
        if(!rewrite.trigger_pattern().empty()) value["condition"] = value["trigger_pattern"] = rewrite.trigger_pattern();
        if(!rewrite.observed_operations().empty()) {
          value["observed_operations"] = Json::Array{};
          for(const auto& operation : rewrite.observed_operations()) value["observed_operations"].push(operation);
        }
      }
    result.push(value);
  }
  return result;
}
Json source_experiences(const Json& artifact) {
  auto result = all_source_experiences(artifact);
  const auto policy = proto<v1::RuntimePolicy>(artifact["policy"]);
  std::map<std::string, std::string> approved;
  for(const auto& ref : policy.options().approved_experiences()) approved[ref.memory_id()] = ref.content_revision();
  std::set<std::string> shadowed_shared;
  for(const auto& relation : policy.snapshot().cross_relations()) {
    const auto own = approved.find(relation.private_memory_id()), shared = approved.find(relation.shared_memory_id());
    if(relation.relation() == "duplicate" && own != approved.end() && shared != approved.end()
        && own->second == relation.private_content_revision() && shared->second == relation.shared_content_revision())
      shadowed_shared.insert(relation.shared_memory_id());
  }
  Json filtered = Json::Array{};
  for(const auto& item : result.items()) if(!shadowed_shared.count(item.str("memory_id"))) filtered.push(item);
  return filtered;
}

// Content-only components retain their original wire form for old searches.
Json experience_component(const Json& source) {
  if(!source.has("trigger_pattern") && !source.has("observed_operations")) return source["content"];
  Json result{{"content", source["content"]}};
  for(const auto* field : {"trigger_pattern", "observed_operations"})
    if(source.has(field)) result[field] = source[field];
  return result;
}
std::string experience_content(const Json& value) {
  return value.null() ? std::string{} : value.object() ? value.str("content") : value.text();
}

Json feedback_trial(const dataset::v1::EvaluationTrial& trial, const Json& controls) {
  const auto raw = json(trial), evidence = json(trial.evidence());
  Json result{{"verdict", trial.verdict()}, {"checks", excerpt(raw["checks"])},
      {"metrics", json(trial.metrics())}, {"execution_status", trial.execution_status()}};
  auto& view = result["evidence"];
  for(const auto* field : {"output", "state", "artifacts", "loop_review"})
    if(evidence.has(field)) view[field] = excerpt(evidence[field]);
  const auto traces = evidence.has("traces") ? evidence["traces"]
      : evidence.has("trace") ? Json(Json::Array{evidence["trace"]}) : Json(Json::Array{});
  for(const auto& trace : traces.items()) {
    Json run{{"run_id", trace["identity"]["run_id"]}, {"actions", Json::Array{}}};
    std::map<std::string, Json> starts, step_starts, action_controls, loop_controls, experience_controls;
    Json target_ledger = Json::Object{}, target_attempts = Json::Object{};
    for(const auto& event : trace["events"].items()) {
      if(event.str("kind") == "action.end") {
        const auto& frame = static_cast<const Json&>(target_attempts)[event.str("action_id")];
        settle_target_event(target_ledger, frame, frame["options"], event);
      }
      if(event.str("kind") == "action.start") starts[event.str("action_id")] = event;
      if(event.str("kind") == "step.start") step_starts[event.str("step_id")] = event;
      if(event.str("kind") == "advice.applied" && event["attributes"]["receipt"].str("boundary") == "llm")
        experience_controls[event.str("step_id")]["application_status"] = event["application"]["status"];
      const auto& decision = event["attributes"]["decision"];
      const auto& query = decision["query"];
      if(event["attributes"].str("type") != "policy.query") continue;
      if(query.str("boundary") == "llm" && !event.str("step_id").empty()) {
        Json advice{{"target_scope", query["advice_query"]["target_scope"]}, {"memory_ids", Json::Array{}}};
        if(decision["experiences"].array())
          for(const auto& source : decision["experiences"].items()) advice["memory_ids"].push(source["memory_id"]);
        if(query.has("observed_operations")) advice["observed_operations"] = query["observed_operations"];
        experience_controls[event.str("step_id")] = advice;
      }
      if(query.str("boundary") == "schedule" && query["actions"].array()) {
        Json plan{{"query_event_id", event["event_id"]}, {"target_scope", query["advice_query"]["target_scope"]},
            {"actions", Json::Array{}}, {"effective_plan", decision["schedule"]}};
        for(const auto& action : query["actions"].items()) {
          Json item{{"action_id", action["action_id"]}, {"operation", action["operation"]}};
          if(action.has("after")) item["after"] = action["after"];
          plan["actions"].push(item);
        }
        run["schedule_plans"].push(plan);
      }
      if(query.str("boundary") == "loop" && !event.str("step_id").empty()) {
        const auto& progress = decision["progress"];
        const auto& previous = query["previous"];
        const auto fingerprint = query.str("state_fingerprint");
        const bool counted = !query.flag("pending_actions", false) && !fingerprint.empty()
            && progress.str("fingerprint") == fingerprint
            && progress["iteration"].dump() == query["iteration"].dump();
        // Expose the runtime counter before a replan resets it. Model-reviewed
        // waste is a different signal; missing/disabled counters stay unknown.
        Json control{{"query_event_id", event["event_id"]}, {"target_scope", query["advice_query"]["target_scope"]},
            {"observed_no_progress", counted ? Json(std::max({query.num("repeated_action_steps", 0), query.num("repeated_failure_steps", 0),
                previous.str("fingerprint") == fingerprint ? previous.num("no_progress", 0) + 1 : std::size_t(0)})) : Json()},
            {"pending_actions", query.flag("pending_actions", false)}, {"proposed_decision", progress["decision"]}};
        control["failed_actions"] = query.num("failed_actions", 0);
        control["repeated_failure_steps"] = query.num("repeated_failure_steps", 0);
        // Protobuf JSON represents uint64 iteration as a decimal string.
        control["iteration"] = static_cast<std::size_t>(proto<v1::RuntimeQuery>(query).iteration());
        if(progress.has("rule_id")) control["rule_id"] = progress["rule_id"];
        loop_controls[event.str("step_id")] = std::move(control);
      }
      if(query.str("boundary") != "actions" || !query["actions"].array()) continue;
      for(const auto& planned : query["actions"].items()) {
        if(planned.str("action_id").empty()) continue;
        Json control{{"query_event_id", event["event_id"]}, {"target_scope", query["advice_query"]["target_scope"]}};
        if(planned.has("schema_required_arguments")) control["schema_required_arguments"] = planned["schema_required_arguments"];
        // Evaluate presence before display truncation can hide a real argument.
        const auto& configured = controls["actions"][planned.str("operation")];
        const auto& required = configured.array() ? configured : planned["schema_required_arguments"];
        control["missing_required_arguments"] = Json::Array{};
        if(required.array()) for(const auto& name : required.items()) {
          const auto& value = planned["arguments"][name.text()];
          if(value.null() || (value.string() && value.text().empty())
              || (value.array() && value.items().empty()) || (value.object() && value.fields().empty()))
            control["missing_required_arguments"].push(name);
        }
        if(planned.has("prior_failures")) control["prior_failures"] = planned["prior_failures"];
        Json observed{{"action_rules", Json::Array{}}};
        if(planned["arguments"].object()) for(const auto& argument : planned["arguments"].fields()) {
          if(observed["action_rules"].items().size() >= 32) break;
          const auto& value = argument.second;
          if(!identifier(argument.first) || value.null() || value.object() || value.array()
              || (value.string() && (value.text().empty() || value.text().size() > 1024))) continue;
          observed["action_rules"].push(Json{{"rule_id", argument.first},
              {"target_scope", query["advice_query"]["target_scope"]}, {"operation", planned["operation"]},
              {"failure_key_arguments", Json::Array{argument.first}}});
        }
        const auto prior = target_failure_snapshot(observed, query["advice_query"].str("target_scope"),
            planned.str("operation"), planned["arguments"], target_ledger);
        target_attempts[planned.str("action_id")] = Json{{"entries", prior}, {"options", observed},
            {"scope", query["advice_query"]["target_scope"]}, {"operation", planned["operation"]}};
        for(const auto& attempt : prior.fields()) if(attempt.second.num("count", 0))
          control["failure_key_candidates"].push(Json{{"argument", attempt.first}, {"prior_failures", attempt.second["count"]},
              {"cross_execution", !attempt.second.str("execution_id").empty()
                  && attempt.second.str("execution_id") != query.str("execution_id")}});
        if(planned.has("target_failures")) control["target_failures"] = planned["target_failures"];
        if(decision["actions"].array()) for(const auto& outcome : decision["actions"].items())
          if(outcome.str("action_id") == planned.str("action_id")) control["decision"] = outcome["decision"];
        action_controls[planned.str("action_id")] = std::move(control);
      }
    }
    for(const auto& event : trace["events"].items()) {
      const auto kind = event.str("kind");
      if(kind != "action.end" && kind != "run.start" && kind != "run.end" && kind != "loop.decision") continue;
      const auto found = starts.find(event.str("action_id"));
      const auto start = found == starts.end() ? Json(Json::Object{}) : found->second;
      Json action = Json::Object{};
      for(const auto* field : {"event_id", "kind", "actor_id", "execution_id", "loop_id", "iteration",
                              "status", "error", "decision", "reason", "outcome"})
        if(event.has(field)) action[field] = excerpt(event[field]);
      const auto operation = event.str("operation", start.str("operation"));
      if(!operation.empty()) action["operation"] = operation;
      if(event["attributes"].has("executed")) action["executed"] = event["attributes"]["executed"];
      const bool model = event["attributes"].str("semantic") == "model"
          || start["attributes"].str("semantic") == "model";
      if(kind == "action.end") {
        action["semantic"] = event["attributes"].str("semantic", start["attributes"].str("semantic"));
        const auto step = step_starts.find(event.str("step_id"));
        if(start["attributes"]["extensions"].flag("guard_only", false)
            || (step != step_starts.end() && step->second["attributes"]["extensions"].flag("guard_only", false)))
          action["guard_only"] = true;
        const auto control = action_controls.find(event.str("action_id"));
        action["action_control"] = control == action_controls.end() ? Json() : control->second;
        if(model) {
          const auto advice = experience_controls.find(event.str("step_id"));
          if(advice != experience_controls.end()) action["experience_query"] = advice->second;
        }
      }
      if(kind == "loop.decision") {
        const auto control = loop_controls.find(event.str("step_id"));
        action["loop_control"] = control == loop_controls.end() ? Json() : control->second;
      }
      // Reflection uses task/rubric, actual decisions and tool outcomes. Replayed
      // model message histories and provider envelopes do not add new actions.
      if(!model) {
        if(event.has("input")) action["input"] = excerpt(event["input"]);
        else if(start.has("input")) action["input"] = excerpt(start["input"]);
      }
      if(event.has("output")) {
        const auto& output = event["output"];
        action["output"] = excerpt(output);
        if(model && output["choices"].array()) {
          action["output"] = Json::Array{};
          for(const auto& choice : output["choices"].items()) {
            Json message = Json::Object{};
            for(const auto* field : {"content", "tool_calls", "function_call", "refusal"})
              if(choice["message"].has(field)) message[field] = excerpt(choice["message"][field]);
            if(choice.has("finish_reason")) message["finish_reason"] = choice["finish_reason"];
            action["output"].push(message);
          }
        }
      }
      run["actions"].push(action);
    }
    view["runs"].push(run);
  }
  return result;
}

bool action_control_evidence(const Json& action) {
  const auto& control = action["action_control"];
  if(action.str("kind") != "action.end" || !control.object()
      || control.str("decision") != "allow") return false;
  if(control.num("prior_failures", 0) >= 2
      && (action.flag("guard_only", false) || action.flag("executed", true))) return true;
  if(action.flag("executed", true) && control["failure_key_candidates"].array())
    for(const auto& target : control["failure_key_candidates"].items())
      if(target.flag("cross_execution", false)) return true;
  return action.flag("executed", true) && control["missing_required_arguments"].array()
      && !control["missing_required_arguments"].items().empty();
}

Json control_feedback_trial(const dataset::v1::EvaluationTrial& trial, const Json& selected, const Json& controls) {
  const auto full = feedback_trial(trial, controls);
  auto chosen = [&](const char* component) {
    return std::any_of(selected.items().begin(), selected.items().end(),
        [&](const auto& item) { return item.text() == component; });
  };
  const bool actions = chosen("action_rules"), loops = chosen("loop_rules"), schedules = chosen("schedule_rules");
  Json checks = Json::Array{};
  for(const auto& check : full["checks"].items()) {
    if(checks.items().size() == 8) break;
    Json item = Json::Object{};
    for(const auto* field : {"check_id", "status", "attempts"}) if(check.has(field)) item[field] = check[field];
    if(check.has("reason")) item["reason"] = clip(check.str("reason"), 512);
    checks.push(item);
  }
  Json result{{"verdict", full["verdict"]}, {"checks", checks}, {"metrics", full["metrics"]},
      {"execution_status", full["execution_status"]}, {"evidence_truncated", true},
      {"control_evidence_preserved", true}};
  auto& evidence = result["evidence"];
  if(loops && full["evidence"].has("loop_review"))
    evidence["loop_review"] = compact_excerpt(full["evidence"]["loop_review"]);
  evidence["runs"] = Json::Array{};
  for(const auto& run : full["evidence"]["runs"].items()) {
    Json view{{"run_id", run["run_id"]}, {"actions", Json::Array{}}};
    if(schedules && run["schedule_plans"].array()) {
      view["schedule_plans"] = Json::Array{};
      std::vector<std::pair<std::size_t, Json>> preferred, other, kept;
      std::size_t ordinal = 0;
      for(const auto& plan : run["schedule_plans"].items()) {
        std::set<std::string> instances;
        for(const auto& action : plan["actions"].items())
          if(!action.str("operation").empty() && !action.str("action_id").empty()) instances.insert(action.str("action_id"));
        auto compact = compact_excerpt(plan);
        // Instance IDs and host dependencies are already a content-free view.
        // Cutting this array can hide the only producer/consumer pair.
        compact["actions"] = plan["actions"];
        (instances.size() > 1 ? preferred : other).emplace_back(ordinal++, std::move(compact));
      }
      for(const auto* rows : {&preferred, &other}) for(const auto& plan : *rows) {
        if(kept.size() == 8) break;
        kept.push_back(plan);
      }
      std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      for(const auto& plan : kept) view["schedule_plans"].push(plan.second);
    }
    std::vector<std::pair<std::size_t, Json>> opportunities, primary, secondary;
    std::size_t ordinal = 0;
    for(const auto& action : run["actions"].items()) {
      const auto order = ordinal++;
      bool keep = false, important = false, actionable = false;
      if(actions && action.str("kind") == "action.end" && action["action_control"].object()) keep = true;
      if(actions && action_control_evidence(action)) actionable = true;
      if(loops && action.str("kind") == "loop.decision" && action["loop_control"].object()) {
        keep = true;
        const auto& control = action["loop_control"];
        important = control.num("observed_no_progress", 0) || control.num("failed_actions", 0)
            || control.num("repeated_failure_steps", 0) || control.str("proposed_decision", "continue") != "continue";
        actionable = important || (controls.flag("aggressive_controls", false)
            && control.num("iteration", 0) > controls.num("min_iterations", 2));
      }
      if((loops || schedules) && action.str("kind") == "action.end"
          && action.str("semantic") != "model" && !action.str("operation").empty()) {
        keep = true;
        important |= action.str("status") == "failed" || action.has("error");
      }
      if(!keep) continue;
      Json item = Json::Object{};
      for(const auto* field : {"event_id", "kind", "actor_id", "execution_id", "loop_id", "iteration", "status",
                              "error", "decision", "reason", "outcome", "operation", "executed",
                              "semantic", "guard_only", "input", "output", "action_control", "loop_control"})
        if(action.has(field)) item[field] = compact_excerpt(action[field]);
      // Small typed evidence is consumed by the selector as well as the model.
      // Do not turn an omitted item into an absent parameter or predicate.
      if(action["action_control"].object()) item["action_control"] = action["action_control"];
      (actionable ? opportunities : important ? primary : secondary).emplace_back(order, std::move(item));
    }
    std::vector<std::pair<std::size_t, Json>> chosen_actions;
    for(const auto* rows : {&opportunities, &primary, &secondary}) for(const auto& action : *rows) {
      if(chosen_actions.size() == 16) break;
      chosen_actions.push_back(action);
    }
    std::sort(chosen_actions.begin(), chosen_actions.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    for(const auto& action : chosen_actions) view["actions"].push(action.second);
    if(view["actions"].items().size() || view["schedule_plans"].array()) evidence["runs"].push(view);
  }
  return result;
}

struct Search {
  fs::path dataset_path, evaluation_path, root;
  Json seed, baseline, evaluation, config, gate, controls, tree;
  std::string context, evaluation_context;
  std::function<bool()> cancelled;
  std::shared_ptr<legacy::ModelAdapter> model;
  dataset::v1::EvaluationDataset dataset;
  std::vector<std::string> train, val, holdout;
  std::size_t reserve = 0;

  void save() { const auto bytes = tree.dump(); require(bytes.size() <= 16777216, "gepa_tree_limit"); write(root / "tree.json", bytes); }
  void check() const {
    require(!cancelled || !cancelled(), "gepa_cancelled");
    require(now() < tree.num("started_ms", 0) + config.num("timeout_ms", 1800000), "gepa_time_budget_exhausted");
  }
  std::size_t deadline() const { return tree.num("started_ms", 0) + config.num("timeout_ms", 1800000); }
  std::size_t holdout_time() const {
    const auto budget = config.num("timeout_ms", 1800000);
    if(config.num("holdout_reserve_ms", 0)) return config.num("holdout_reserve_ms", 0);
    // Evaluation wall time includes host execution, grading and persistence.
    // Its observed throughput already accounts for evaluator parallelism.
    const auto trials = tree.num("timed_trials", 0);
    const auto estimate = trials ? (tree.num("evaluation_wall_ms", 0) * reserve * 3 / (trials * 2)) : 0;
    return std::min(budget - 1, std::max(budget / 5, estimate));
  }
  void search_check(std::size_t trials = 0) const {
    check();
    const auto measured = tree.num("timed_trials", 0);
    const auto estimate = measured ? tree.num("evaluation_wall_ms", 0) * trials / measured : 0;
    require(now() + estimate < deadline() - holdout_time(), "gepa_search_time_reserved");
  }
  Json artifact(std::size_t index) const {
    const auto value = Json::parse(read(root / "versions" / (std::to_string(index) + ".json")));
    require(agent_version_id(value.dump()) == tree["nodes"].items().at(index).str("version"), "gepa_version_changed"); return value;
  }
  Report evaluate(const Json& candidate, const std::vector<std::string>& ids, const std::string& split, bool final = false) {
    Json settings = evaluation, selected = Json::Array{};
    for(const auto& id : ids) selected.push(id);
    settings["splits"] = Json::Array{split}; settings["task_ids"] = selected;
    auto host = evaluation["variants"].items().front(), a = host, b = host;
    a["id"] = "baseline"; a["artifact"] = baseline; a["version"] = agent_version_id(baseline.dump());
    b["id"] = "candidate"; b["artifact"] = candidate; b["version"] = agent_version_id(candidate.dump());
    settings["variants"] = final ? Json(Json::Array{a, b}) : Json(Json::Array{b});
    const auto key = hash(context + settings.dump());
    auto cache = static_cast<const Json&>(tree)["evaluations"][key];
    if(cache.str("status") == "complete" && (final || config.flag("cache_evaluations", true))) {
      const auto bytes = read(root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0))) / "report.json", 134217728);
      require(hash(bytes) == cache.str("report_hash"), "gepa_report_changed");
      const auto report = proto<Report>(Json::parse(bytes));
      // Older complete entries may still lack an enabled objective metric.
      if(final || scores(report).fields().size() == ids.size()) return report;
    }
    check();
    if(!final && config.flag("cache_evaluations", true)) {
      std::map<std::string, std::vector<dataset::v1::EvaluationTrial>> reused;
      Json sources = Json::Array{}, source_refs = Json::Array{}; Report joined;
      for(const auto& entry : static_cast<const Json&>(tree)["evaluations"].fields()) {
        const auto& previous = entry.second;
        if((previous.str("status") != "complete" && previous.str("status") != "inconclusive")
            || previous.str("split") != split
            || previous.str("version") != b.str("version") || previous.has("baseline_version")) continue;
        bool overlaps = false;
        for(const auto& id : previous["task_ids"].items())
          overlaps |= std::find(ids.begin(), ids.end(), id.text()) != ids.end() && !reused.count(id.text());
        if(!overlaps) continue;
        const auto source_report = fs::path("evaluations") / entry.first / ("attempt-" + std::to_string(previous.num("attempt", 0))) / "report.json";
        const auto bytes = read(root / source_report, 134217728);
        require(hash(bytes) == previous.str("report_hash"), "gepa_report_changed");
        const auto report = proto<Report>(Json::parse(bytes));
        if(previous.str("status") == "complete") require(scored(report), "gepa_report_changed");
        if(!report.complete()) continue;
        // An exact checkpoint can regrade failed judgments without rerunning
        // the agent. Splitting it into a fresh subset loses that checkpoint.
        if(entry.first == key && std::any_of(report.trials().begin(), report.trials().end(), [](const auto& trial) {
          return trial.execution_status() == "completed"
              && (trial.error_code() == "evaluation_judge_failed"
                  || (trial.verdict() != "passed" && trial.verdict() != "failed"));
        })) { reused.clear(); break; }
        std::map<std::string, std::vector<dataset::v1::EvaluationTrial>> grouped;
        for(const auto& trial : report.trials()) if(trial.variant_id() == "candidate"
            && std::find(ids.begin(), ids.end(), trial.task_id()) != ids.end() && !reused.count(trial.task_id()))
          grouped[trial.task_id()].push_back(trial);
        bool used = false;
        for(auto& item : grouped) {
          std::set<unsigned> repetitions; for(const auto& trial : item.second) repetitions.insert(trial.repetition());
          if(item.second.size() == evaluation.num("repetitions", 1) && repetitions.size() == item.second.size()) {
            // A failed sibling does not invalidate this task. Reuse only its
            // complete repetitions with all objective metrics still observed.
            Report task_report; task_report.set_complete(true);
            for(const auto& trial : item.second) *task_report.add_trials() = trial;
            if(scores(task_report).fields().size() != 1) continue;
            reused.emplace(item.first, std::move(item.second)); used = true;
          }
        }
        if(used) {
          sources.push(entry.first);
          // Same-key retries refer to the earlier attempt, never their own report.
          source_refs.push(Json{{"key", entry.first}, {"report", source_report.string()}, {"report_hash", previous["report_hash"]}});
          if(joined.schema_version() == 0) joined = report;
        }
      }
      if(!reused.empty()) {
        std::vector<std::string> missing; for(const auto& id : ids) if(!reused.count(id)) missing.push_back(id);
        Report fresh; bool complete = true;
        if(!missing.empty()) { fresh = evaluate(candidate, missing, split); complete = fresh.complete(); }
        const auto reused_count = reused.size() * evaluation.num("repetitions", 1);
        for(const auto& trial : fresh.trials()) reused[trial.task_id()].push_back(trial);
        if(!missing.empty()) {
          auto subset = settings; subset["task_ids"] = Json::Array{};
          for(const auto& id : missing) subset["task_ids"].push(id);
          const auto fresh_key = hash(context + subset.dump());
          const auto& added = static_cast<const Json&>(tree)["evaluations"][fresh_key];
          sources.push(fresh_key);
          source_refs.push(Json{{"key", fresh_key}, {"report", added["report"]}, {"report_hash", added["report_hash"]}});
        }
        joined.clear_trials();
        for(const auto& id : ids) for(const auto& trial : reused[id]) *joined.add_trials() = trial;
        complete &= static_cast<std::size_t>(joined.trials_size()) == ids.size() * evaluation.num("repetitions", 1);
        joined.set_report_kind("gepa_cached_evaluation.v1"); joined.set_run_id("gepa-cache-" + key);
        joined.set_config_fingerprint(key); joined.set_complete(complete); joined.clear_comparisons();
        auto configuration = json(joined.configuration()); configuration["task_ids"] = selected;
        *joined.mutable_configuration() = proto<google::protobuf::Struct>(configuration);
        *joined.mutable_summary() = proto<google::protobuf::Struct>(Json{{"trials", std::size_t(joined.trials_size())},
            {"reused_trials", reused_count}, {"source_reports", sources}, {"source_report_refs", source_refs}});
        cache["attempt"] = cache.num("attempt", 0) + 1;
        cache["status"] = scores(joined).fields().size() == ids.size() ? "complete" : "inconclusive";
        cache["version"] = b["version"]; cache["split"] = split; cache["task_ids"] = selected;
        cache["trials_reserved"] = 0; cache["source_reports"] = sources;
        cache["source_report_refs"] = source_refs;
        const auto directory = fs::path("evaluations") / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
        const auto bytes = json(joined).dump(); write(root / directory / "report.json", bytes);
        write(root / directory / "report.pb", joined.SerializeAsString());
        cache["report_hash"] = hash(bytes); cache["report"] = (directory / "report.json").string();
        tree["evaluations"][key] = cache; save(); return joined;
      }
    }
    TaskEvaluationOptions options; options.config_json = settings.dump();
    if(cache.num("attempt", 0) > 0 && (final || config.flag("cache_evaluations", true))) {
      const auto previous = root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
      // Resume validates context, manifest and artifacts, retaining execution
      // evidence while retrying only failed judgments or unfinished trials.
      if(fs::exists(previous / "manifest.json")) options.resume_report = previous / "report.json";
    }
    auto cost = ids.size() * evaluation.num("repetitions", 1) * (final ? 2 : 1);
    if(!options.resume_report.empty()) {
      const auto previous = proto<Report>(Json::parse(read(options.resume_report, 134217728)));
      std::size_t reusable = 0;
      for(const auto& trial : previous.trials()) {
        if(trial.execution_status() != "completed" && trial.execution_status() != "failed") continue;
        const auto evidence = json(trial.evidence());
        const bool reviewed = !evaluation["judge"].has("loop_review_prompt")
            || evidence["loop_review"].str("status") == "complete";
        const bool judge_error = trial.error_code() == "evaluation_judge_failed"
            && std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto& check) {
              return check.check_id() == "model" && check.status() == "error";
            });
        reusable += judge_error || (reviewed && trial.error_code() != "evaluation_judge_failed"
            && (trial.verdict() == "passed" || trial.verdict() == "failed"));
      }
      cost -= std::min(cost, reusable);
    }
    if(!final) search_check(cost);
    require(tree.num("trials_used", 0) + cost + (final ? 0 : reserve) <= config.num("max_trials", 400), "gepa_trial_budget_exhausted");
    tree["trials_used"] = tree.num("trials_used", 0) + cost;
    cache["attempt"] = cache.num("attempt", 0) + 1; cache["status"] = "running"; cache["trials_reserved"] = cost;
    cache["version"] = b["version"]; cache["split"] = split; cache["task_ids"] = selected;
    if(final) cache["baseline_version"] = a["version"];
    tree["evaluations"][key] = cache;
    save();
    const auto cutoff = final ? deadline() : deadline() - holdout_time();
    options.cancelled = [&] { return (cancelled && cancelled()) || now() >= cutoff; };
    const auto directory = root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
    const auto evaluation_started = now();
    Report report;
    try { report = run_task_evaluation(dataset_path, evaluation_path, directory, options); }
    catch(...) {
      cache["status"] = "error"; tree["evaluations"][key] = cache; save(); throw;
    }
    cache["status"] = scored(report) && (final || scores(report).fields().size() == ids.size()) ? "complete" : "inconclusive";
    cache["report_hash"] = hash(read(directory / "report.json", 134217728));
    cache["report"] = (fs::path("evaluations") / key / directory.filename() / "report.json").string();
    tree["evaluations"][key] = cache;
    if(report.complete()) {
      tree["evaluation_wall_ms"] = tree.num("evaluation_wall_ms", 0) + now() - evaluation_started;
      tree["timed_trials"] = tree.num("timed_trials", 0) + cost;
    }
    save();
    check();
    if(!final && now() >= cutoff) throw Error("gepa_search_time_reserved");
    for(const auto& trial : report.trials()) for(const auto& check : trial.checks())
      if(check.check_id() == "model" && check.status() == "error" && check.reason() == "evaluation_judge_http_429")
        throw Error("gepa_model_rate_limited");
    // Recover a transient judgment or runner failure once. Checkpoint reuse
    // preserves successful trials, so an isolated runner failure is replayed alone.
    if(!cache.flag("automatic_recovery_used", false) && (final || config.flag("cache_evaluations", true))
        && fs::exists(directory / "manifest.json")
        && tree.num("trials_used", 0) + cost + (final ? 0 : reserve) <= config.num("max_trials", 400)) {
      const std::set<std::string> transient{"evaluation_judge_transport_6", "evaluation_judge_transport_7",
          "evaluation_judge_transport_18", "evaluation_judge_transport_28", "evaluation_judge_transport_52",
          "evaluation_judge_transport_55", "evaluation_judge_transport_56", "evaluation_judge_http_408",
          "evaluation_judge_http_500", "evaluation_judge_http_502", "evaluation_judge_http_503",
          "evaluation_judge_http_504", "invalid_json"};
      bool regrade = false;
      for(const auto& trial : report.trials()) {
      const std::set<std::string> runner{"runner_nonzero_exit", "runner_wait_failed", "runner_poll_failed",
          "runner_read_failed", "runner_cleanup_timeout", "runner_timeout"};
        if(trial.execution_status() != "completed") {
          if(runner.count(trial.error_code())) { regrade = true; continue; }
          return report;
        }
        if(trial.error_code().empty()) continue;
        if(trial.error_code() != "evaluation_judge_failed") return report;
        const auto failed = std::find_if(trial.checks().begin(), trial.checks().end(), [](const auto& check) {
          return check.check_id() == "model" && check.status() == "error";
        });
        const auto invalid_loop_review = failed != trial.checks().end()
            && failed->reason().rfind("evaluation_loop_review_invalid:", 0) == 0;
        if(failed == trial.checks().end()
            || (!transient.count(failed->reason()) && !invalid_loop_review)) return report;
        regrade = true;
      }
      if(regrade) {
        // Manual cancellation/resume does not spend the automatic recovery.
        cache["automatic_recovery_used"] = true;
        tree["evaluations"][key] = cache; save();
        return evaluate(candidate, ids, split, final);
      }
    }
    return report;
  }
  Json scores(const Report& report) const {
    Json result = Json::Object{};
    if(!scored(report)) return result;
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto metrics = json(trial.metrics()), evidence = json(trial.evidence());
      bool passed = trial.verdict() == "passed";
      // Reuse measured baseline executions, but apply current deterministic
      // contracts to their saved traces before comparing candidate quality.
      for(const auto& task : dataset.tasks()) if(task.task_id() == trial.task_id()) {
        passed &= schedule_contract_check(json(task.attributes()), evidence).str("status") != "failed";
        break;
      }
      const auto loops = evidence.has("loop_review")
          ? (evidence["loop_review"].str("status") == "complete" ? evidence["loop_review"]["count"] : Json())
          : evidence["trajectory"]["summary"]["invalid_loop_lower_bound"];
      const auto& objective = config["objective"];
      const auto lw = number(objective["invalid_loop_weight"], 0.2);
      const auto tw = number(objective["token_weight"], 0.05);
      const auto dw = number(objective["latency_weight"], 0);
      const bool need_loops = lw || gate.has("min_invalid_loop_reduction");
      const bool unknown_tokens = metrics["tokens"].null();
      if((need_loops && loops.null()) || (tw && unknown_tokens
          && config.str("unknown_token_policy", "reject") != "worst_case")) return Json(Json::Object{});
      auto penalty = [](double x, double scale) { return x / (scale + x); };
      // This bounded cost has supremum 1. An explicit conservative policy
      // uses that bound for ranking; it never invents a measured token count.
      const auto token_cost = unknown_tokens ? 1.0
          : penalty(number(metrics["tokens"]), number(objective["token_scale"], 10000));
      const double fitness = (passed ? 1.0 : 0.0)
          - lw * penalty(number(loops), number(objective["invalid_loop_scale"], 1))
          - tw * token_cost
          - dw * penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      auto& row = result[trial.task_id()];
      row["fitness"] = number(row["fitness"]) + fitness;
      row["quality"] = number(row["quality"]) + (passed ? 1 : 0);
      row["duration_ms"] = number(row["duration_ms"]) + trial.duration_ms();
      if(!unknown_tokens) {
        row["tokens"] = number(row["tokens"]) + number(metrics["tokens"]);
        row["token_observations"] = row.num("token_observations", 0) + 1;
      }
      if(need_loops) {
        const auto method = evidence.has("loop_review") ? "model_review" : "rule_lower_bound";
        if(row.has("loop_method") && row.str("loop_method") != method) return Json(Json::Object{});
        row["loop_method"] = method;
        row["invalid_loops"] = number(row["invalid_loops"]) + number(loops);
      }
      if(lw) row["loop_efficiency"] = number(row["loop_efficiency"]) + 1 - penalty(number(loops), number(objective["invalid_loop_scale"], 1));
      if(tw) row["token_efficiency"] = number(row["token_efficiency"]) + 1 - token_cost;
      if(tw && unknown_tokens) row["unknown_token_trials"] = row.num("unknown_token_trials", 0) + 1;
      if(dw) row["latency_efficiency"] = number(row["latency_efficiency"]) + 1 - penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      row["repetitions"] = row.num("repetitions", 0) + 1;
    }
    for(auto& item : result.fields()) {
      item.second["fitness"] = number(item.second["fitness"]) / item.second.num("repetitions", 1);
      item.second["quality"] = number(item.second["quality"]) / item.second.num("repetitions", 1);
      for(const auto* field : {"invalid_loops", "loop_efficiency", "token_efficiency", "latency_efficiency", "duration_ms"})
        if(item.second.has(field)) item.second[field] = number(item.second[field]) / item.second.num("repetitions", 1);
      if(item.second.has("tokens"))
        item.second["tokens"] = number(item.second["tokens"]) / item.second.num("token_observations", 1);
    }
    return result;
  }
  static double mean(const Json& values, const char* field) {
    double sum = 0; for(const auto& item : values.fields()) sum += number(item.second[field]);
    return values.fields().empty() ? -1e9 : sum / values.fields().size();
  }
  Json optimization_status(const Json& candidate) const {
    const auto& base = static_cast<const Json&>(tree)["baseline_validation"];
    Json reasons = Json::Array{}, hard = Json::Array{}, criteria = Json::Object{};
    double gap = 0;
    auto reject = [&](bool condition, const char* code, double distance, bool hard_constraint) {
      if(!condition) return;
      reasons.push(code); if(hard_constraint) hard.push(code);
      gap += std::max(0.0, distance);
    };
    bool paired = candidate.object() && base.object() && !candidate.fields().empty()
        && candidate.fields().size() == base.fields().size();
    if(paired) for(const auto& item : base.fields()) paired &= candidate.has(item.first);
    reject(!paired, "validation_pair_incomplete", 1, true);
    const auto quality = paired ? mean(candidate, "quality") : -1;
    const auto base_quality = paired ? mean(base, "quality") : -1;
    const auto minimum_quality = number(gate["min_pass_rate"], 0.8);
    const auto allowed_drop = number(gate["max_pass_rate_drop"], 0);
    criteria["quality"] = quality; criteria["baseline_quality"] = base_quality;
    criteria["minimum_quality"] = minimum_quality; criteria["maximum_quality_drop"] = allowed_drop;
    reject(paired && quality < minimum_quality - 1e-12, "quality_below_threshold",
        (minimum_quality - quality) / std::max(0.1, minimum_quality), true);
    reject(paired && quality + allowed_drop < base_quality - 1e-12, "quality_regression",
        (base_quality - allowed_drop - quality) / std::max(0.1, base_quality), true);
    if(gate.has("max_latency_ratio") && paired) {
      const auto latency = mean(candidate, "duration_ms"), base_latency = mean(base, "duration_ms");
      const auto ratio = number(gate["max_latency_ratio"], 1);
      criteria["duration_ms"] = latency; criteria["baseline_duration_ms"] = base_latency;
      criteria["maximum_latency_ratio"] = ratio;
      reject(base_latency <= 0 || latency < 0 || latency > base_latency * ratio,
          "max_latency_ratio", base_latency > 0 && ratio > 0 ? latency / (base_latency * ratio) - 1 : 1, true);
    }
    if(gate.has("max_token_ratio") && paired) {
      bool observed = true;
      for(const auto& item : base.fields()) observed &= item.second.has("tokens")
          && item.second.num("token_observations", 0) == item.second.num("repetitions", 0)
          && candidate[item.first].has("tokens")
          && candidate[item.first].num("token_observations", 0) == candidate[item.first].num("repetitions", 0);
      const auto tokens = observed ? mean(candidate, "tokens") : -1;
      const auto base_tokens = observed ? mean(base, "tokens") : -1;
      const auto ratio = number(gate["max_token_ratio"], 1);
      criteria["tokens"] = observed ? Json(tokens) : Json();
      criteria["baseline_tokens"] = observed ? Json(base_tokens) : Json();
      criteria["maximum_token_ratio"] = ratio;
      reject(!observed || base_tokens <= 0 || tokens > base_tokens * ratio,
          !observed ? "token_metric_unobserved" : "max_token_ratio",
          observed && base_tokens > 0 && ratio > 0 ? tokens / (base_tokens * ratio) - 1 : 1, true);
    }
    if(gate.has("min_invalid_loop_reduction") && paired) {
      bool observed = true; std::string method;
      for(const auto& item : base.fields()) {
        const auto& value = candidate[item.first];
        observed &= item.second.has("invalid_loops") && value.has("invalid_loops")
            && !item.second["invalid_loops"].null() && !value["invalid_loops"].null()
            && item.second.str("loop_method") == value.str("loop_method");
        if(method.empty()) method = item.second.str("loop_method");
        observed &= item.second.str("loop_method") == method;
      }
      const auto loops = observed ? mean(candidate, "invalid_loops") : -1;
      const auto base_loops = observed ? mean(base, "invalid_loops") : -1;
      const auto reduction = number(gate["min_invalid_loop_reduction"], 0);
      const auto limit = base_loops * (1 - reduction);
      criteria["invalid_loops"] = observed ? Json(loops) : Json();
      criteria["baseline_invalid_loops"] = observed ? Json(base_loops) : Json();
      criteria["minimum_loop_reduction"] = reduction;
      reject(!observed, "loop_metric_unobserved", 1, false);
      reject(observed && reduction > 0 && base_loops <= 0,
          "loop_baseline_has_no_observed_waste", 1, false);
      reject(observed && base_loops > 0 && loops > limit + 1e-12,
          "loop_target_not_met", (loops - limit) / std::max(1.0, base_loops), false);
    }
    std::size_t supported = 0;
    if(paired) for(const auto& item : base.fields()) {
      const auto& value = candidate[item.first]; bool stable = number(value["quality"]) + 1e-12 >= number(item.second["quality"]);
      if(gate.has("max_latency_ratio")) stable &= number(value["duration_ms"])
          <= number(item.second["duration_ms"]) * number(gate["max_latency_ratio"], 1) + 1e-12;
      if(gate.has("max_token_ratio")) stable &= value.has("tokens") && item.second.has("tokens")
          && number(value["tokens"]) <= number(item.second["tokens"]) * number(gate["max_token_ratio"], 1) + 1e-12;
      if(gate.has("min_invalid_loop_reduction")) stable &= value.has("invalid_loops") && item.second.has("invalid_loops")
          && number(value["invalid_loops"]) <= number(item.second["invalid_loops"]) + 1e-12;
      supported += stable;
    }
    const auto robustness = paired ? double(supported) / base.fields().size() : 0;
    criteria["paired_nonregression"] = robustness;
    const bool feasible = paired && hard.items().empty();
    const bool passed = feasible && reasons.items().empty();
    return Json{{"passed", passed}, {"feasible", feasible}, {"score", 1.0 / (1.0 + gap)},
        {"robustness", robustness}, {"distance", gap}, {"reasons", reasons}, {"hard_reasons", hard}, {"criteria", criteria}};
  }
  Json annotate_validation(Json& values) const {
    const auto status = optimization_status(values);
    for(auto& item : values.fields()) {
      item.second["goal_score"] = status["score"];
      item.second["feasible_score"] = status.flag("feasible", false) ? 1 : 0;
      item.second["robustness_score"] = status["robustness"];
    }
    return status;
  }
  bool improves(const Json& child, const Json& parent) const {
    // The deployment gate compares raw loop means, not bounded efficiencies.
    // More correct answers must not hide a regression in the configured loop objective.
    if(!gepa::loops_no_worse(child, parent)) return false;
    const auto difference = mean(child, "quality") - mean(parent, "quality");
    return difference > 1e-12 || (difference >= -1e-12
        && mean(child, "fitness") > mean(parent, "fitness") + number(config["min_improvement"], 0.000001));
  }
  std::vector<std::size_t> frontier() const {
    return gepa::pool(gepa::frontier_map(tree["nodes"], val, config));
  }
  std::vector<std::string> minibatch(std::size_t round) const {
    const auto count = std::min(config.num("minibatch_size", 4), train.size());
    const auto batches = (train.size() + count - 1) / count;
    const bool epochs = config.str("minibatch_strategy", "epoch_shuffled") == "epoch_shuffled";
    auto shuffled = train; std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + (epochs ? round / batches : 0)));
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    std::vector<std::string> selected;
    if(epochs) {
      const auto start = (round % batches) * count;
      for(std::size_t i = start; i < std::min(start + count, shuffled.size()); ++i) selected.push_back(shuffled[i]);
    } else for(std::size_t i = 0; i < count; ++i) selected.push_back(shuffled[(round * count + i) % shuffled.size()]);
    std::sort(selected.begin(), selected.end()); return selected;
  }
  Json components(const Json& candidate) const {
    Json result = Json::Object{};
    // Keep the seed-authorized axes when a candidate disables an experience.
    const auto authorized = source_experiences(seed);
    for(const auto& item : authorized.items()) result["experience:" + item.str("memory_id")] = nullptr;
    const auto experiences = source_experiences(candidate);
    for(const auto& item : experiences.items()) result["experience:" + item.str("memory_id")] = experience_component(item);
    const auto policy = proto<v1::RuntimePolicy>(candidate["policy"]);
    if(controls.flag("enabled", false) && policy.options().requested_level() >= v1::L2) {
      // Generic schema rules use the current request's tool contract and do
      // not require a configured framework/tool allowlist.
      result["action_rules"] = Json::Array{};
      for(const auto& rule : policy.options().action_rules()) if(rule.has_source_experience()) {
        Json arguments = Json::Array{}; for(const auto& key : rule.required_arguments()) arguments.push(key);
        Json item{{"memory_id", rule.source_experience().memory_id()}, {"operation", rule.operation()},
            {"required_arguments", arguments}, {"max_failed_attempts", std::size_t(rule.max_failed_attempts())},
            {"reason", rule.reason()}, {"match_mode", rule.match_mode()}};
        if(rule.failure_key_arguments_size()) {
          item["failure_key_arguments"] = Json::Array{};
          for(const auto& key : rule.failure_key_arguments()) item["failure_key_arguments"].push(key);
        }
        result["action_rules"].push(std::move(item));
      }
      if(std::find(policy.options().host_capabilities().begin(), policy.options().host_capabilities().end(), "schedule.dispatch")
          != policy.options().host_capabilities().end()) {
        result["schedule_rules"] = Json::Array{};
        for(const auto& rule : policy.options().schedule_rules()) if(rule.has_source_experience()) {
          Json task_terms = Json::Array{}, exclude_terms = Json::Array{};
          for(const auto& term : rule.task_terms()) task_terms.push(term);
          for(const auto& term : rule.exclude_terms()) exclude_terms.push(term);
          result["schedule_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()},
              {"before_operation", rule.before_operation()}, {"after_operation", rule.after_operation()},
              {"require_success", rule.require_success()}, {"reason", rule.reason()},
              {"task_terms", task_terms}, {"exclude_terms", exclude_terms}});
        }
      }
      if(policy.options().requested_level() >= v1::L3 && without_generated_controls(policy.options()).loop_rules_size()) {
        result["loop_rules"] = Json::Array{};
        for(const auto& rule : policy.options().loop_rules()) if(rule.has_source_experience())
          result["loop_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()}, {"actor_id", rule.actor_id()}, {"max_iterations", std::size_t(rule.max_iterations())},
              {"max_no_progress", std::size_t(rule.max_no_progress())}, {"max_replans", std::size_t(rule.max_replans())}, {"replan_on_failure", rule.replan_on_failure()},
              {"reason", rule.reason()}, {"match_mode", rule.match_mode()}});
      }
    }
    return result;
  }
  Json compose(const Json& parent, const Json& values) const {
    const auto original = components(parent);
    require(values.object() && original.fields().size() == values.fields().size(), "gepa_components_changed");
    for(const auto& item : original.fields()) require(values.has(item.first), "gepa_components_changed");
    validate_local_data(values);
    auto result = parent; const auto policy = proto<v1::RuntimePolicy>(parent["policy"]); auto options = policy.options();
    options.clear_experience_overrides();
    options.clear_approved_experiences();
    const auto experiences = source_experiences(seed);
    for(const auto& item : experiences.items()) {
      if(values["experience:" + item.str("memory_id")].null()) continue;
      const auto id = item.str("memory_id");
      const auto& value = values["experience:" + id];
      const auto content = experience_content(value);
      require(!content.empty() && content.size() <= config.num("max_experience_bytes", 2048), "gepa_experience_limit");
      for(const auto& entry : policy.snapshot().experiences()) if(entry.memory_id() == id) {
        auto* approved = options.add_approved_experiences();
        approved->set_memory_id(id); approved->set_content_revision(entry.content_revision());
        const auto pattern = value.str("trigger_pattern");
        if(entry.content() != content || !pattern.empty() || value["observed_operations"].array()) {
          auto* rewrite = options.add_experience_overrides(); rewrite->set_content(content);
          *rewrite->mutable_source_experience() = *approved;
          if(pattern != entry.trigger_pattern()) rewrite->set_trigger_pattern(pattern);
          if(value["observed_operations"].array())
            for(const auto& operation : value["observed_operations"].items()) rewrite->add_observed_operations(operation.text());
        }
      }
    }
    // A private/public duplicate is one searchable component. Preserve the
    // shadowed public approval while that component is enabled, and remove both
    // copies together during an ablation.
    const auto seed_policy = proto<v1::RuntimePolicy>(seed["policy"]);
    // Control evidence is immutable, not an injectable experience component.
    // Preserve its seed approval through mutations and multi-parent merges.
    for(const auto& source : seed_policy.snapshot().experiences()) {
      if(source.control_target_type().empty()) continue;
      for(const auto& approved : seed_policy.options().approved_experiences())
        if(approved.memory_id() == source.memory_id()
            && approved.content_revision() == source.content_revision())
          *options.add_approved_experiences() = approved;
    }
    for(const auto& relation : seed_policy.snapshot().cross_relations()) {
      const auto key = "experience:" + relation.private_memory_id();
      if(relation.relation() != "duplicate" || !values.has(key) || values[key].null()) continue;
      for(const auto& approved : seed_policy.options().approved_experiences())
        if(approved.memory_id() == relation.shared_memory_id()
            && approved.content_revision() == relation.shared_content_revision())
          *options.add_approved_experiences() = approved;
    }
    std::sort(options.mutable_experience_overrides()->begin(), options.mutable_experience_overrides()->end(),
        [](const auto& a, const auto& b) { return a.source_experience().memory_id() < b.source_experience().memory_id(); });
    if(values.has("action_rules") || values.has("loop_rules") || values.has("schedule_rules")) {
      Json draft{{"action_rules", values.has("action_rules") ? values["action_rules"] : Json(Json::Array{})},
          {"loop_rules", values.has("loop_rules") ? values["loop_rules"] : Json(Json::Array{})},
          {"schedule_rules", values.has("schedule_rules") ? values["schedule_rules"] : Json(Json::Array{})}};
      options = apply_control_draft(policy.snapshot(), without_generated_controls(options), controls, draft);
    }
    result["policy"] = json(compile_runtime_policy(policy.snapshot(), options));
    require(published_policy_compatible(proto<v1::RuntimePolicy>(result["policy"]),
        proto<v1::RuntimePolicy>(seed["policy"]), controls.dump()), "gepa_candidate_not_authorized");
    agent_version_id(result.dump()); return result;
  }
  int schedule_training_priority(const std::string& task_id) const {
    for(const auto& task : dataset.tasks()) if(task.task_id() == task_id && task.split() == "train") {
      const auto attributes = json(task.attributes());
      const auto dependency = attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
      if(dependency == "independent" || dependency == "parallel" || dependency == "none") return 0;
      if(!dependency.empty()) return std::holds_alternative<bool>(attributes["stop_on_failure"].value)
          && attributes["stop_on_failure"].boolean() ? 3 : 2;
      // This is only a train-task sampling hint. A real batch, differential
      // preflight and paired evaluation still decide whether an edge is valid.
      auto text = task.task_input();
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      });
      auto has = [&](const char* phrase) { return text.find(phrase) != std::string::npos; };
      if((has("先") && has("再")) || has("完成后") || has(" before ")
          || has(" after ") || has(" then ")) return 1;
      if(has("互不依赖") || has("无需依赖") || has("no dependency")
          || has("independent") || has("in parallel")) return 0;
      return has("依赖") || has("基于") ? 1 : 0;
    }
    return 0;
  }
  Json packet(const Json& parent, const Report& feedback, const Json& selected) const {
    const auto available = components(parent), sources = source_experiences(seed);
    Json selected_values = Json::Object{};
    bool control_selected = false;
    for(const auto& item : selected.items()) {
      require(available.has(item.text()), "gepa_component_not_authorized");
      selected_values[item.text()] = available[item.text()];
      control_selected |= item.text() == "action_rules" || item.text() == "loop_rules" || item.text() == "schedule_rules";
    }
    Json contract = Json::Object{};
    for(const auto* key : {"min_pass_rate", "max_pass_rate_drop", "min_invalid_loop_reduction",
             "max_latency_ratio", "max_token_ratio"})
      if(gate.has(key)) contract[key] = gate[key];
    Json result{{"parent_version", agent_version_id(parent.dump())}, {"experiences", Json::Array{}},
        {"components", selected_values}, {"selected_components", selected}, {"feedback", Json::Array{}},
        {"acceptance", config.str("acceptance", "strict_improvement")},
        {"objective", config["objective"]}, {"optimization_contract", contract},
        {"limits", Json{{"max_experience_bytes", config.num("max_experience_bytes", 2048)}}}};
    const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
    for(const auto& source : sources.items()) {
      const auto key = "experience:" + source.str("memory_id");
      if(!control_selected && !selected_values.has(key)) continue;
      auto experience = source;
      // Control generation needs source meaning because experience components
      // are intentionally absent. Experience mutation already carries the
      // current text in components, so do not duplicate it.
      if(!control_selected || selected_values.has(key)) experience.fields().erase("content");
      result["experiences"].push(experience);
    }
    for(auto& experience : result["experiences"].items()) {
      for(const auto& source : policy.snapshot().experiences())
        if(source.memory_id() == experience.str("memory_id")
            && selected_values.has("experience:" + source.memory_id())
            && experience_content(selected_values["experience:" + source.memory_id()]) != source.content())
          experience["original_content"] = source.content();
    }
    if(controls.flag("enabled", false) && control_selected) {
      result["controls"] = control_generation_input(policy.snapshot(), without_generated_controls(policy.options()), controls);
      result["controls"].fields().erase("memories");  // Sources already appear above; do not duplicate the snapshot.
    }
    // Only training content reaches reflection. Validation/holdout scores select
    // candidates but their tasks, rubrics and traces never enter this packet.
    std::vector<const dataset::v1::EvaluationTrial*> trials;
    for(const auto& trial : feedback.trials()) trials.push_back(&trial);
    const auto feedback_scores = scores(feedback);
    std::map<std::string, bool> relevant;
    if(selected.items().size() == 1) for(const auto* trial : trials) {
      Json single{{"feedback", Json::Array{Json{{"trial", feedback_trial(*trial, controls)}}}}};
      relevant[trial->trial_id()] = control_evidence(single, selected.items().front().text());
    }
    if(selected.items().size() == 1 && selected.items().front().text() == "schedule_rules"
        && std::any_of(trials.begin(), trials.end(), [&](const auto* trial) {
          return relevant[trial->trial_id()] && schedule_training_priority(trial->task_id()) > 0;
        }))
      trials.erase(std::remove_if(trials.begin(), trials.end(), [&](const auto* trial) {
        return !relevant[trial->trial_id()] || schedule_training_priority(trial->task_id()) == 0;
      }), trials.end());
    std::stable_sort(trials.begin(), trials.end(), [&](const auto* a, const auto* b) {
      if(relevant[a->trial_id()] != relevant[b->trial_id()]) return relevant[a->trial_id()];
      if(selected.items().size() == 1 && selected.items().front().text() == "schedule_rules"
          && schedule_training_priority(a->task_id()) != schedule_training_priority(b->task_id()))
        return schedule_training_priority(a->task_id()) > schedule_training_priority(b->task_id());
      if((a->verdict() == "failed") != (b->verdict() == "failed")) return a->verdict() == "failed";
      // Spend the bounded reflection context on the weakest observed tasks,
      // including wasted loops in otherwise successful executions.
      return number(feedback_scores[a->task_id()]["fitness"]) < number(feedback_scores[b->task_id()]["fitness"]);
    });
    for(const auto* trial : trials) {
      if(result["feedback"].items().size() >= config.num("max_feedback_trials", 4)) break;
      const dataset::v1::EvaluationTask* task = nullptr;
      for(const auto& value : dataset.tasks()) if(value.task_id() == trial->task_id()) task = &value;
      require(task && task->split() == "train", "gepa_reflection_split_violation");
      Json turns = Json::Array{}, constraints = Json::Array{};
      for(const auto& turn : task->followup_inputs()) turns.push(clip(turn, 4096));
      for(const auto& constraint : task->constraints()) constraints.push(clip(constraint, 4096));
      const auto trial_view = control_selected ? control_feedback_trial(*trial, selected, controls) : feedback_trial(*trial, controls);
      Json row{{"task_id", task->task_id()}, {"task_input", clip(task->task_input(), 4096)},
          {"followup_inputs", turns}, {"constraints", constraints},
          {"expected_behavior", clip(task->expected_behavior(), 4096)}, {"trial", trial_view}};
      auto trial_packet = result; trial_packet["feedback"].push(row);
      if(trial_packet.dump().size() > config.num("max_input_bytes", 65536)) {
        if(control_selected) {
          row["task_input"] = clip(task->task_input(), 1024);
          row["followup_inputs"] = Json::Array{};
          for(const auto& turn : task->followup_inputs()) {
            if(row["followup_inputs"].items().size() == 4) break;
            row["followup_inputs"].push(clip(turn, 1024));
          }
          row["constraints"] = Json::Array{};
          for(const auto& constraint : task->constraints()) {
            if(row["constraints"].items().size() == 8) break;
            row["constraints"].push(clip(constraint, 1024));
          }
          row["expected_behavior"] = clip(task->expected_behavior(), 2048);
        } else {
          row["trial"] = Json{{"verdict", trial->verdict()}, {"checks", excerpt(json(*trial)["checks"])},
              {"output", excerpt(json(trial->evidence())["output"])}, {"metrics", json(trial->metrics())}, {"evidence_truncated", true}};
        }
        trial_packet = result; trial_packet["feedback"].push(row);
      }
      if(trial_packet.dump().size() <= config.num("max_input_bytes", 65536)) result = std::move(trial_packet);
    }
    require(!result["feedback"].items().empty() && result.dump().size() <= config.num("max_input_bytes", 65536), "gepa_feedback_budget_too_small");
    validate_local_data(result); return result;
  }
  bool control_evidence(const Json& input, const std::string& component) const {
    for(const auto& feedback : input["feedback"].items()) {
      // Budget-limited rows retain scores and output, not executable evidence.
      const auto& runs = feedback["trial"]["evidence"]["runs"];
      if(!runs.array()) continue;
      for(const auto& run : runs.items()) {
        if(component == "schedule_rules" && run["schedule_plans"].array())
          for(const auto& plan : run["schedule_plans"].items()) {
            std::set<std::string> instances;
            if(plan["actions"].array()) for(const auto& action : plan["actions"].items())
              if(!action.str("operation").empty() && !action.str("action_id").empty()) instances.insert(action.str("action_id"));
            if(instances.size() > 1) return true;
          }
        if(!run["actions"].array()) continue;
        for(const auto& action : run["actions"].items()) {
          if(component == "action_rules" && action_control_evidence(action)) return true;
          if(component == "loop_rules" && action.str("kind") == "loop.decision"
              && action["loop_control"].object()
              && !action["loop_control"].flag("pending_actions", false)
              && (action["loop_control"].str("proposed_decision", "continue") == "continue"
                  || action["loop_control"].str("proposed_decision") == "replan")) {
            if(seed["policy"]["options"].flag("aggressive_controls", false)
                && action["loop_control"].num("iteration", 0) > controls.num("min_iterations", 2)) return true;
            if(!action["loop_control"]["observed_no_progress"].null()
                && action["loop_control"].num("observed_no_progress", 0) >= controls.num("min_no_progress", 1)) return true;
            if(action["loop_control"].num("repeated_failure_steps", 0)) {
              const auto policy = proto<v1::RuntimePolicy>(seed["policy"]);
              const v1::LoopRule* base = nullptr;
              for(const auto& rule : policy.options().loop_rules())
                if(!rule.has_source_experience() && rule.target_scope() == action["loop_control"].str("target_scope")
                    && (rule.actor_id().empty() || rule.actor_id() == action.str("actor_id"))
                    && (!base || !rule.actor_id().empty())) base = &rule;
              if(base && base->no_progress_decision() == "replan" && base->max_replans()) return true;
            }
          }
        }
      }
    }
    return false;
  }
  std::set<std::string> applied_experiences(const Report& report) const {
    std::map<std::string, std::string> canonical;
    const auto policy = proto<v1::RuntimePolicy>(seed["policy"]);
    std::set<std::string> searchable;
    const auto sources = source_experiences(seed);
    for(const auto& source : sources.items()) searchable.insert(source.str("memory_id"));
    for(const auto& relation : policy.snapshot().cross_relations())
      if(relation.relation() == "duplicate" && searchable.count(relation.private_memory_id()))
        canonical[relation.shared_memory_id()] = relation.private_memory_id();
    std::set<std::string> result;
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto evidence = json(trial.evidence());
      const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      for(const auto& trace : traces.items()) for(const auto& event : trace["events"].items()) {
        if(event.str("kind") != "advice.applied" || event["application"].str("status") != "applied") continue;
        if(event["attributes"]["receipt"].str("boundary") != "llm") continue;
        for(const auto& source : event["attributes"]["receipt"]["experiences"].items()) {
          auto id = source.str("memory_id");
          if(canonical.count(id)) id = canonical[id];
          if(!id.empty()) result.insert("experience:" + id);
        }
      }
    }
    return result;
  }
  Json observed_experience(const Json& available, const std::set<std::string>& observed) const {
    std::string best; std::size_t uses = SIZE_MAX;
    for(const auto& key : observed) if(available.has(key)) {
      const auto count = tree["component_selections"].num(key, 0);
      if(count < uses || (count == uses && key < best)) { best = key; uses = count; }
    }
    return best.empty() ? Json(Json::Array{}) : Json(Json::Array{best});
  }
  Json applied_changes(const Json& parent, const Report& report) const {
    Json changed = Json::Object{};
    auto effect = [](const Json& decision, const std::string& boundary) {
      if(boundary == "llm") return Json(decision.str("context"));
      if(boundary == "loop") return Json(decision["progress"].str("decision"));
      Json result = Json::Object{};
      if(boundary == "actions" && decision["actions"].array())
        for(const auto& action : decision["actions"].items()) result[action.str("action_id")] = action["decision"];
      if(boundary == "schedule" && decision["schedule"]["actions"].array())
        for(const auto& action : decision["schedule"]["actions"].items()) {
          result[action.str("action_id")] = Json::Object{};
          if(action["after"].array()) for(const auto& edge : action["after"].items())
            result[action.str("action_id")][edge.str("action_id")] = edge.flag("require_success", false);
        }
      return result;
    };
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto evidence = json(trial.evidence());
      const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      for(const auto& trace : traces.items()) {
        if(!trace["events"].array()) continue;
        const auto bound = bind_runtime_policy(proto<v1::RuntimePolicy>(parent["policy"]), proto<v1::Identity>(trace["identity"]));
        const PolicyRuntime runtime(bound);
        std::map<std::string, std::string> observed;
        std::set<std::string> executed, completed_schedules, committed;
        for(const auto& event : trace["events"].items()) {
          if(event.str("kind") == "advice.applied")
            observed[event["application"].str("query_id")] = event["application"].str("status");
          if(event.str("kind") == "action.end" && event["attributes"].flag("executed", false)) executed.insert(event.str("step_id"));
          if(event.str("kind") == "step.end" && event.str("status") == "completed") completed_schedules.insert(event.str("step_id"));
          if(event.str("kind") == "loop.decision") committed.insert(event.str("step_id"));
        }
        Json ledger = Json::Object{}, attempts = Json::Object{};
        for(const auto& event : trace["events"].items()) {
          if(event.str("kind") == "action.end")
            settle_target_event(ledger, static_cast<const Json&>(attempts)[event.str("action_id")], json(bound.options()), event);
          if(event["attributes"].str("type") != "policy.query") continue;
          const auto& actual = event["attributes"]["decision"];
          auto q = proto<v1::RuntimeQuery>(actual["query"]); const auto boundary = q.boundary();
          if(boundary != "llm" && boundary != "actions" && boundary != "loop" && boundary != "schedule") continue;
          if(q.actor_id().empty()) q.set_actor_id(event.str("actor_id"));
          if(q.previous().iteration()) q.mutable_previous()->set_policy_id(bound.policy_id());
          for(auto& action : *q.mutable_actions()) if(boundary == "actions") {
            const auto snapshot = target_failure_snapshot(json(bound.options()), q.advice_query().target_scope(),
                action.operation(), json(action.arguments()), ledger);
            attempts[action.action_id()] = Json{{"entries", snapshot}, {"scope", q.advice_query().target_scope()}, {"operation", action.operation()}};
            action.clear_target_failures();
            for(const auto& entry : snapshot.fields()) (*action.mutable_target_failures())[entry.first] = entry.second.num("count", 0);
          }
          if(effect(actual, boundary).dump() == effect(json(runtime.decide(q)), boundary).dump()) continue;
          const auto status = observed[q.advice_query().query_id()];
          // A no-op receipt is expected when removing an injection/constraint.
          // Count it only with a completed real boundary, never a failed receipt.
          bool applied = status == "applied";
          if(actual.str("status") == "no_change" && status == "rejected") {
            if(boundary == "llm" || boundary == "actions") applied = executed.count(event.str("step_id"));
            if(boundary == "loop") applied = committed.count(event.str("step_id"));
            if(boundary == "schedule") applied = completed_schedules.count(event.str("step_id"));
          }
          if(applied) changed[trial.task_id()][boundary] = changed[trial.task_id()].num(boundary, 0) + 1;
        }
      }
    }
    return changed;
  }
  bool observed_local_gain(const Json& candidate, const Json& parent, const Json& changes) const {
    Json selected = Json::Object{}, previous = Json::Object{};
    for(const auto& task : changes.fields()) if(candidate.has(task.first) && parent.has(task.first)) {
      selected[task.first] = candidate[task.first]; previous[task.first] = parent[task.first];
    }
    return gepa::local_gain(selected, previous, number(config["min_improvement"], 0.000001));
  }
  void remember_opportunities(const Report& report) {
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate" || std::find(train.begin(), train.end(), trial.task_id()) == train.end()) continue;
      Json input{{"feedback", Json::Array{Json{{"trial", feedback_trial(trial, controls)}}}}};
      for(const auto* component : {"schedule_rules", "action_rules", "loop_rules"})
        if(control_evidence(input, component)) {
          auto& opportunity = tree["training_opportunities"][component][trial.task_id()];
          const auto previous = number(opportunity, opportunity.null() ? 0 : 1);
          opportunity = std::max(previous, trial.verdict() == "failed" ? 2.0 : 1.0);
        }
    }
  }
  std::vector<std::string> component_batch(const Json& selected, std::size_t sequence) const {
    auto result = minibatch(sequence);
    if(selected.items().size() != 1) return result;
    const auto& available = tree["training_opportunities"][selected.items().front().text()];
    std::vector<std::string> candidates;
    if(available.object()) for(const auto& item : available.fields())
      if(std::find(train.begin(), train.end(), item.first) != train.end()) candidates.push_back(item.first);
    if(selected.items().front().text() == "schedule_rules") for(const auto& id : train)
      if(schedule_training_priority(id) && std::find(candidates.begin(), candidates.end(), id) == candidates.end())
        candidates.push_back(id);
    if(candidates.empty()) return result;
    std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
      if(selected.items().front().text() == "schedule_rules"
          && schedule_training_priority(a) != schedule_training_priority(b))
        return schedule_training_priority(a) > schedule_training_priority(b);
      return number(available[a], available[a].null() ? 0 : 1)
          > number(available[b], available[b].null() ? 0 : 1);
    });
    if(selected.items().front().text() == "schedule_rules" && schedule_training_priority(candidates.front()) >= 2)
      candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto& id) {
        return schedule_training_priority(id) == 0;
      }), candidates.end());
    // Keep epoch coverage while reserving one example for the selected capability.
    const auto& task = candidates[sequence % candidates.size()];
    if(std::find(result.begin(), result.end(), task) == result.end()) result.front() = task;
    std::sort(result.begin(), result.end()); return result;
  }
  Json control_preflight(const Json& parent, const Json& candidate, const Report& report) const {
    Json result{{"rules", Json::Object{}}, {"missing", Json::Array{}}};
    std::set<std::string> existing;
    const auto old = proto<v1::RuntimePolicy>(parent["policy"]), policy = proto<v1::RuntimePolicy>(candidate["policy"]);
    for(const auto& rule : old.options().action_rules()) existing.insert(rule.rule_id());
    for(const auto& rule : old.options().loop_rules()) existing.insert(rule.rule_id());
    for(const auto& rule : old.options().schedule_rules()) existing.insert(rule.rule_id());
    for(const auto& rule : policy.options().action_rules()) if(rule.has_source_experience() && !existing.count(rule.rule_id()))
      result["rules"][rule.rule_id()] = 0;
    for(const auto& rule : policy.options().loop_rules()) if(rule.has_source_experience() && !existing.count(rule.rule_id()))
      result["rules"][rule.rule_id()] = 0;
    for(const auto& rule : policy.options().schedule_rules()) if(rule.has_source_experience() && !existing.count(rule.rule_id()))
      result["rules"][rule.rule_id()] = 0;
    if(!result["rules"].fields().empty()) for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto evidence = json(trial.evidence());
      const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      for(const auto& trace : traces.items()) {
        if(!trace["events"].array()) continue;
        const auto bound = bind_runtime_policy(policy, proto<v1::Identity>(trace["identity"]));
        const PolicyRuntime runtime(bound);
        Json ledger = Json::Object{}, attempts = Json::Object{};
        for(const auto& event : trace["events"].items()) {
          if(event.str("kind") == "action.end")
            settle_target_event(ledger, static_cast<const Json&>(attempts)[event.str("action_id")], json(bound.options()), event);
          if(event["attributes"].str("type") != "policy.query") continue;
          const auto& before = event["attributes"]["decision"];
          auto query = proto<v1::RuntimeQuery>(before["query"]);
          if(query.actor_id().empty()) query.set_actor_id(event.str("actor_id"));
          if(query.boundary() != "llm" && query.boundary() != "loop"
              && query.boundary() != "actions" && query.boundary() != "schedule") continue;
          if(query.previous().iteration()) query.mutable_previous()->set_policy_id(bound.policy_id());
          for(auto& action : *query.mutable_actions()) if(query.boundary() == "actions") {
            const auto snapshot = target_failure_snapshot(json(bound.options()), query.advice_query().target_scope(),
                action.operation(), json(action.arguments()), ledger);
            attempts[action.action_id()] = Json{{"entries", snapshot}, {"scope", query.advice_query().target_scope()}, {"operation", action.operation()}};
            action.clear_target_failures();
            for(const auto& entry : snapshot.fields()) (*action.mutable_target_failures())[entry.first] = entry.second.num("count", 0);
          }
          const auto after = runtime.decide(query);
          if(query.boundary() == "llm" && query.observed_operations().empty()
              && after.context() != before.str("context")) {
            auto lower = [](std::string text) {
              std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
              });
              return text;
            };
            const auto task = lower(query.advice_query().task());
            for(const auto& rule : bound.options().schedule_rules()) {
              if(!result["rules"].has(rule.rule_id()) || rule.task_terms().empty()
                  || rule.target_scope() != query.advice_query().target_scope()) continue;
              bool matched = true;
              for(const auto& term : rule.task_terms()) matched &= task.find(lower(term)) != std::string::npos;
              for(const auto& term : rule.exclude_terms()) matched &= task.find(lower(term)) == std::string::npos;
              if(matched && after.context().find(rule.before_operation()) != std::string::npos
                  && after.context().find(rule.after_operation()) != std::string::npos)
                result["rules"][rule.rule_id()] = result["rules"].num(rule.rule_id(), 0) + 1;
            }
          }
          if(query.boundary() == "schedule" && before["schedule"]["actions"].array()) {
            auto edges = [](const v1::SchedulePlan& plan) {
              Json result = Json::Object{};
              for(const auto& action : plan.actions()) {
                result[action.action_id()] = Json::Object{};
                for(const auto& dependency : action.after())
                  result[action.action_id()][dependency.action_id()] = dependency.require_success();
              }
              return result.dump();
            };
            if(edges(after.schedule()) != edges(proto<v1::SchedulePlan>(before["schedule"])))
              for(const auto& id : after.schedule().rule_ids()) if(result["rules"].has(id))
                result["rules"][id] = result["rules"].num(id, 0) + 1;
          }
          for(const auto& action : after.actions()) if(action.decision() == "reject" && result["rules"].has(action.rule_id())) {
            bool allowed = true;
            for(const auto& previous : before["actions"].items())
              if(previous.str("action_id") == action.action_id()) allowed = previous.str("decision") == "allow";
            if(allowed) result["rules"][action.rule_id()] = result["rules"].num(action.rule_id(), 0) + 1;
          }
          const auto& progress = after.progress();
          if(query.boundary() == "loop" && before["progress"].str("decision") != progress.decision()
              && progress.decision() != "continue" && result["rules"].has(progress.rule_id()))
            result["rules"][progress.rule_id()] = result["rules"].num(progress.rule_id(), 0) + 1;
        }
      }
    }
    for(const auto& rule : result["rules"].fields()) if(!rule.second.integer()) result["missing"].push(rule.first);
    result["passed"] = result["missing"].items().empty(); return result;
  }
  Json repair_schedule_contracts(const Json& draft, const Report& report, Json& repairs) const {
    repairs = Json::Array{};
    if(!draft["controls"]["schedule_rules"].array()) return draft;
    auto result = draft;
    auto lower = [](std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      });
      return value;
    };
    for(auto& rule : result["controls"]["schedule_rules"].items()) {
      if(rule.flag("require_success", false) || !rule["task_terms"].array()
          || rule["task_terms"].items().empty()) continue;
      for(const auto& trial : report.trials()) {
        if(trial.variant_id() != "candidate" || trial.split() != "train") continue;
        const dataset::v1::EvaluationTask* task = nullptr;
        for(const auto& candidate : dataset.tasks()) if(candidate.task_id() == trial.task_id()) { task = &candidate; break; }
        if(!task) continue;
        const auto attributes = json(task->attributes());
        const auto dependency = attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
        if(!std::holds_alternative<bool>(attributes["stop_on_failure"].value)
            || !attributes["stop_on_failure"].boolean() || dependency.empty()
            || dependency == "independent" || dependency == "parallel" || dependency == "none") continue;
        const auto evidence = json(trial.evidence());
        const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        bool witnessed = false;
        for(const auto& trace : traces.items()) for(const auto& event : trace["events"].items()) {
          if(event["attributes"].str("type") != "policy.query") continue;
          const auto& q = event["attributes"]["decision"]["query"];
          if(q.str("boundary") != "schedule") continue;
          bool before = false, after = false, matches = true;
          for(const auto& action : q["actions"].items()) {
            before |= action.str("operation") == rule.str("before_operation");
            after |= action.str("operation") == rule.str("after_operation");
          }
          const auto text = lower(q["advice_query"].str("task"));
          for(const auto& term : rule["task_terms"].items()) matches &= text.find(lower(term.text())) != std::string::npos;
          if(rule["exclude_terms"].array()) for(const auto& term : rule["exclude_terms"].items())
            matches &= text.find(lower(term.text())) == std::string::npos;
          witnessed |= before && after && matches;
        }
        if(!witnessed) continue;
        // A declared training constraint is authoritative. Do not infer this
        // from a failed provider response or rewrite independent branches.
        rule["require_success"] = true;
        rule["reason"] = "前置操作成功后才启动依赖操作；前置失败时仅跳过依赖分支。";
        repairs.push(Json{{"task_id", trial.task_id()}, {"before_operation", rule["before_operation"]},
            {"after_operation", rule["after_operation"]}, {"constraint", "stop_on_failure"}});
        break;
      }
    }
    return result;
  }
  Json repair_schedule_guards(const Json& draft, const Report& report, Json& repairs) const {
    // Calibrate only against labelled training batches; validation and holdout
    // remain selection evidence and never rewrite a candidate's task guard.
    auto result = draft;
    repairs = Json::Array{};
    if(!result["controls"]["schedule_rules"].array()) return result;
    for(auto& rule : result["controls"]["schedule_rules"].items()) {
      if(!rule["task_terms"].array() || rule["task_terms"].items().size() < 3) continue;
      const auto before_operation = rule.str("before_operation"), after_operation = rule.str("after_operation");
      std::vector<std::string> positive, negative;
      for(const auto& trial : report.trials()) {
        if(trial.variant_id() != "candidate") continue;
        const dataset::v1::EvaluationTask* task = nullptr;
        for(const auto& item : dataset.tasks()) if(item.task_id() == trial.task_id()) { task = &item; break; }
        if(!task) continue;
        const auto attributes = json(task->attributes());
        const auto dependency = attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
        if(dependency.empty()) continue;
        const bool independent = dependency == "independent" || dependency == "parallel" || dependency == "none";
        const auto evidence = json(trial.evidence());
        const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        for(const auto& trace : traces.items()) {
          if(!trace["events"].array()) continue;
          for(const auto& event : trace["events"].items()) {
            if(event["attributes"].str("type") != "policy.query") continue;
            const auto& query = event["attributes"]["decision"]["query"];
            if(query.str("boundary") != "schedule" || !query["actions"].array()) continue;
            int before = 0, after = 0;
            for(const auto& action : query["actions"].items()) {
              before += action.str("operation") == before_operation;
              after += action.str("operation") == after_operation;
            }
            if(before && after && (before_operation != after_operation || before >= 2))
              (independent ? negative : positive).push_back(query["advice_query"].str("task"));
          }
        }
      }
      if(positive.empty()) continue;
      auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
          return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        return text;
      };
      std::vector<std::string> retained;
      for(const auto& task : positive) {
        std::vector<std::string> matched;
        const auto text = lower(task);
        for(const auto& term : rule["task_terms"].items())
          if(text.find(lower(term.text())) != std::string::npos) matched.push_back(term.text());
        if(matched.size() > retained.size()) retained = std::move(matched);
      }
      if(retained.size() < 2 || retained.size() == rule["task_terms"].items().size()) continue;
      bool unsafe = false;
      for(const auto& task : negative) {
        const auto text = lower(task);
        bool matches = true;
        for(const auto& term : retained) matches &= text.find(lower(term)) != std::string::npos;
        if(rule["exclude_terms"].array()) for(const auto& term : rule["exclude_terms"].items())
          if(text.find(lower(term.text())) != std::string::npos) matches = false;
        unsafe |= matches;
      }
      if(unsafe) continue;
      const auto removed = rule["task_terms"].items().size() - retained.size();
      rule["task_terms"] = Json::Array{};
      for(const auto& term : retained) rule["task_terms"].push(term);
      repairs.push(Json{{"before_operation", before_operation}, {"after_operation", after_operation},
          {"removed_terms", removed}, {"matched_terms", retained.size()}});
    }
    return result;
  }
  Json mutate(const Json& parent, const Json& draft, const Json& selected) const {
    keys(draft, {"reason", "experience", "experiences", "controls"}); validate_local_data(draft);
    require(!draft.str("reason").empty() && draft.str("reason").size() <= 2048
        && !(draft.has("experience") && draft.has("experiences"))
        && (draft.has("experience") || draft.has("experiences") || draft.has("controls")), "gepa_draft_invalid");
    const auto before = components(parent); auto values = before;
    Json changes = draft.has("experiences") ? draft["experiences"] : Json(Json::Array{});
    if(draft.has("experience")) changes.push(draft["experience"]);
    require(changes.array() && changes.items().size() <= before.fields().size(), "gepa_draft_invalid");
    std::set<std::string> unique;
    for(const auto& change : changes.items()) {
      keys(change, {"memory_id", "content", "trigger_pattern", "observed_operations"});
      require(change.object() && change.has("memory_id") && change.has("content")
          && !change.str("memory_id").empty()
          && (change["content"].null() || (change["content"].string()
              && !change["content"].text().empty()
              && change["content"].text().size() <= config.num("max_experience_bytes", 2048))),
          "gepa_draft_invalid");
      const auto key = "experience:" + change.str("memory_id");
      require(values.has(key), "gepa_experience_not_authorized");
      require(unique.insert(key).second, "gepa_draft_invalid");
      if(change["content"].null()) {
        require(!change.has("trigger_pattern") && !change.has("observed_operations"), "gepa_draft_invalid");
        values[key] = nullptr; continue;
      }
      auto value = values[key];
      if(value.null()) {
        const auto sources = source_experiences(seed);
        for(const auto& source : sources.items())
          if(source.str("memory_id") == change.str("memory_id")) value = experience_component(source);
      }
      if(value.object()) value["content"] = change["content"];
      else value = change["content"];
      for(const auto* field : {"trigger_pattern", "observed_operations"}) if(change.has(field)) {
        if(!value.object()) value = Json{{"content", value}};
        value[field] = change[field];
      }
      if(value.has("trigger_pattern")) require(value["trigger_pattern"].string()
          && value.str("trigger_pattern").size() <= 2048, "gepa_draft_invalid");
      if(value.has("observed_operations")) {
        require(value["observed_operations"].array() && value["observed_operations"].items().size() <= 16, "gepa_draft_invalid");
        std::set<std::string> operations;
        for(const auto& operation : value["observed_operations"].items())
          require(operation.string() && identifier(operation.text()) && operations.insert(operation.text()).second, "gepa_draft_invalid");
        value["observed_operations"] = Json::Array{};
        for(const auto& operation : operations) value["observed_operations"].push(operation);
      }
      values[key] = value;
    }
    if(draft.has("controls")) {
      keys(draft["controls"], {"action_rules", "loop_rules", "schedule_rules"});
      require(draft["controls"].object() && !draft["controls"].fields().empty(), "gepa_draft_invalid");
      for(const auto& item : draft["controls"].fields()) {
        require(item.second.array() && (values.has(item.first) || item.second.items().empty()), "gepa_component_not_authorized");
        if(values.has(item.first)) values[item.first] = item.second;
      }
    }
    std::set<std::string> allowed; for(const auto& item : selected.items()) allowed.insert(item.text());
    for(const auto& item : values.fields()) if(item.second.dump() != before[item.first].dump())
      require(allowed.count(item.first), "gepa_component_not_authorized");
    // An empty draft is a legitimate abstention, not a new agent version.
    // Preserve the original envelope, including its public version identifier.
    if(values.dump() == before.dump()) return parent;
    const auto candidate = compose(parent, values);
    return components(candidate).dump() == before.dump() ? parent : candidate;
  }
  Json reflect(const Json& input, std::size_t node, const std::string& suffix = {}, bool aggregate = false) {
    require(input.dump().size() <= config.num("max_input_bytes", 65536), "gepa_feedback_budget_too_small");
    search_check(); require(tree.num("model_calls", 0) < config.num("max_model_calls", 8), "gepa_model_budget_exhausted");
    tree["model_calls"] = tree.num("model_calls", 0) + 1;
    tree["nodes"].items()[node]["status"] = "reflecting"; save();
    auto adapter = model;
    if(!adapter) {
      auto runtime = legacy::load_runtime_configuration(config.str("runtime_config")); auto http = runtime.model;
      if(http.endpoint.empty()) if(const auto* base = std::getenv("OPENAI_BASE_URL")) {
        http.endpoint = base; while(!http.endpoint.empty() && http.endpoint.back() == '/') http.endpoint.pop_back(); http.endpoint += "/chat/completions";
      }
      if(http.model_id.empty()) if(const auto* name = std::getenv("OPENAI_MODEL")) http.model_id = name;
      require(!http.endpoint.empty() && !http.model_id.empty(), "gepa_model_not_configured");
      if(!runtime.no_api_key) {
        const auto* key = std::getenv((runtime.api_key_env.empty() ? "OPENAI_API_KEY" : runtime.api_key_env).c_str());
        require(key && *key, "gepa_model_key_missing"); http.api_key = key;
      }
      http.max_retries = 0; // One reserved reflection call is one transport attempt.
      search_check();
      http.timeout_ms = std::max<std::size_t>(1, std::min<std::size_t>(http.timeout_ms,
          deadline() - holdout_time() - now()));
      const auto cutoff = deadline() - holdout_time();
      http.cancelled = [&, cutoff] { return (cancelled && cancelled()) || now() >= cutoff; };
      http.max_request_bytes = std::max<std::size_t>(http.max_request_bytes, config.num("max_input_bytes", 65536) * 2);
      adapter = legacy::make_openai_compatible_model_adapter(http);
    }
    legacy::ModelRequest request;
    const auto name = std::to_string(node) + suffix;
    request.packet_id = "gepa-" + context.substr(0, 16) + "-" + name;
    request.system_prompt = read(config.str(aggregate ? "aggregation_prompt_file" : "prompt_file"), 65536); request.user_prompt = input.dump();
    validate_local_data(Json(request.system_prompt));
    write(root / "reflections" / (name + ".request.json"), Json{{"system", request.system_prompt}, {"input", input}}.dump());
    const auto response = adapter->evaluate(request); search_check();
    if(response.error_code == "http_429") {
      // The provider rejected this call; retry the same generation, retaining
      // its reserved budget and already completed execution checkpoints.
      tree["nodes"].items()[node]["status"] = "pending";
      save(); throw Error("gepa_model_rate_limited");
    }
    require(response.transport_ok && response.error_code.empty() && response.body.size() <= 262144, "gepa_reflection_transport_failed");
    bool response_recovered = false, content_recovered = false;
    auto body = parse_model_object(response.body, &response_recovered);
    if(body.has("choices")) {
      require(body["choices"].array() && body["choices"].items().size() == 1, "gepa_reflection_choice_invalid");
      const auto& choice = body["choices"].items().front();
      require(choice.str("finish_reason") == "stop", "gepa_reflection_incomplete");
      body = parse_model_object(choice["message"].str("content"), &content_recovered);
    }
    if(response_recovered || content_recovered) {
      tree["nodes"].items()[node]["response_recovery"] = Json{{"method", "single_json_object"},
          {"provider_envelope", response_recovered}, {"message_content", content_recovered}};
      save();
    }
    validate_local_data(body); return body;
  }
  Json propose(const Json& parent, const Json& input, std::size_t node) {
    const auto n = input["feedback"].items().size(), k = static_cast<std::size_t>(std::sqrt(double(n)));
    if(config.str("reflection_strategy", "single") != "combee" || k < 2) return reflect(input, node);
    const auto& selected = input["selected_components"];
    const auto needed = selected.items().size() * (k + 1);
    require(tree.num("model_calls", 0) + needed <= config.num("max_model_calls", 8), "gepa_model_budget_exhausted");
    auto values = components(parent);
    Json drafts{{"reason", "分组反思并汇总训练证据，保留各组件的受限改进。"}, {"experiences", Json::Array{}}};
    Json stages = Json::Array{};
    std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + node));
    std::size_t component_index = 0;
    for(const auto& component : selected.items()) {
      const auto key = component.text(), prefix = ".combee-" + std::to_string(component_index++);
      std::vector<std::size_t> shuffled;
      for(std::size_t repeat = 0; repeat < config.num("duplication_factor", 2); ++repeat)
        for(std::size_t i = 0; i < n; ++i) shuffled.push_back(i);
      std::shuffle(shuffled.begin(), shuffled.end(), random);
      Json intermediates = Json::Array{};
      const Json one = Json::Array{key};
      for(std::size_t group = 0; group < k; ++group) {
        auto packet = input; packet["selected_components"] = one; packet["feedback"] = Json::Array{};
        packet["phase"] = "combee_map";
        for(std::size_t i = group; i < shuffled.size(); i += k) packet["feedback"].push(input["feedback"].items()[shuffled[i]]);
        // Shuffling duplicated records can make one group larger than the source
        // packet. Keep concrete feedback while respecting the same byte budget.
        if(packet.dump().size() > config.num("max_input_bytes", 65536)) {
          for(auto& row : packet["feedback"].items()) {
            const auto trial = row["trial"];
            row["trial"] = Json{{"verdict", trial["verdict"]}, {"checks", trial["checks"]},
                {"output", trial.has("output") ? trial["output"] : trial["evidence"]["output"]},
                {"metrics", trial["metrics"]}, {"evidence_truncated", true}};
          }
          while(packet["feedback"].items().size() > 1 && packet.dump().size() > config.num("max_input_bytes", 65536)) {
            packet["feedback"].items().pop_back(); packet["feedback_truncated"] = true;
          }
        }
        const auto suffix = prefix + "-map-" + std::to_string(group);
        const auto draft = reflect(packet, node, suffix);
        write(root / "reflections" / (std::to_string(node) + suffix + ".draft.json"), draft.dump());
        const auto candidate = mutate(parent, draft, one);
        intermediates.push(Json{{"reason", draft["reason"]}, {"component", key}, {"value", components(candidate)[key]}});
        stages.push(Json{{"component", key}, {"phase", "map"}, {"file", std::to_string(node) + suffix}, {"feedback_records", packet["feedback"].items().size()}});
        tree["nodes"].items()[node]["reflection_stages"] = stages; save();
      }
      auto packet = input; packet.fields().erase("feedback");
      packet["phase"] = "combee_reduce"; packet["selected_components"] = one; packet["intermediate_proposals"] = intermediates;
      const auto draft = reflect(packet, node, prefix + "-reduce", true);
      write(root / "reflections" / (std::to_string(node) + prefix + "-reduce.draft.json"), draft.dump());
      const auto candidate = mutate(parent, draft, one); values[key] = components(candidate)[key];
      stages.push(Json{{"component", key}, {"phase", "reduce"}, {"file", std::to_string(node) + prefix + "-reduce"}, {"groups", k}});
      tree["nodes"].items()[node]["reflection_stages"] = stages; save();
      if(key.compare(0, 11, "experience:") == 0) {
        auto change = values[key].object() ? values[key] : Json{{"content", values[key]}};
        change["memory_id"] = key.substr(11); drafts["experiences"].push(change);
      } else drafts["controls"][key] = values[key];
    }
    return drafts;
  }
  std::size_t winner() const {
    std::size_t best = 0;
    const auto& nodes = tree["nodes"].items();
    auto preferable = [&](const Json& candidate, const Json& current) {
      const auto& a = candidate["optimization"]; const auto& b = current["optimization"];
      if(gate.flag("require_level_coverage", true)) {
        const bool complete_a = candidate["control_coverage"].flag("level_complete", false);
        const bool complete_b = current["control_coverage"].flag("level_complete", false);
        if(complete_a != complete_b) return complete_a;
      }
      if(a.flag("feasible", false) != b.flag("feasible", false)) return a.flag("feasible", false);
      if(a.flag("passed", false) != b.flag("passed", false)) return a.flag("passed", false);
      const auto goal = number(a["score"]) - number(b["score"]);
      if(std::abs(goal) > 1e-12) return goal > 0;
      const auto robustness = number(a["robustness"]) - number(b["robustness"]);
      if(std::abs(robustness) > 1e-12) return robustness > 0;
      return improves(candidate["validation"], current["validation"]);
    };
    for(std::size_t i = 1; i < nodes.size(); ++i) if(nodes[i].str("status") == "accepted"
        && nodes[i]["control_coverage"].flag("complete", true) && preferable(nodes[i], nodes[best])) best = i;
    return best;
  }
  void proposal(std::size_t index, const Json& candidate) {
    auto& node = tree["nodes"].items()[index]; const auto version = agent_version_id(candidate.dump());
    node["version"] = version; node["status"] = "proposed";
    auto executable = [&](const Json& artifact) {
      auto values = components(artifact);
      for(const auto* category : {"action_rules", "loop_rules", "schedule_rules"})
        if(values[category].array()) for(auto& rule : values[category].items()) rule.fields().erase("reason");
      return values.dump();
    };
    const auto behavior = executable(candidate);
    for(std::size_t j = 0; j < index; ++j) {
      const auto& previous = tree["nodes"].items()[j];
      if(previous.str("version").empty()) continue;
      // Components are read through protobuf: omitted and explicit defaults
      // compare equally, without invalidating previously exported bundles.
      if(previous.str("version") == version || executable(artifact(j)) == behavior) {
        node["duplicate_of"] = j; node["status"] = "duplicate";
        node["skip_reason"] = components(artifact(j)).dump() == components(candidate).dump()
            ? "no_behavior_change" : "no_control_predicate_change"; break;
      }
    }
    write(root / "versions" / (std::to_string(index) + ".json"), candidate.dump()); save();
  }
  void error(std::size_t index, const std::exception& exception) {
    const std::string code = exception.what();
    if(code == "gepa_cancelled" || code == "gepa_time_budget_exhausted"
        || code == "gepa_model_rate_limited") { save(); throw Error(code.c_str()); }
    auto& node = tree["nodes"].items()[index]; node["error_stage"] = node["status"]; node["status"] = "error";
    // Persist fixed codes only; model/transport diagnostics can contain secrets.
    const std::set<std::string> codes{"gepa_trial_budget_exhausted", "gepa_search_time_reserved", "gepa_parent_evaluation_inconclusive",
        "gepa_feedback_budget_too_small", "gepa_reflection_transport_failed", "gepa_reflection_incomplete",
        "gepa_experience_not_authorized", "gepa_experience_limit", "gepa_draft_invalid", "gepa_candidate_not_authorized",
        "gepa_model_not_configured", "gepa_model_key_missing", "credential_value_rejected", "sensitive_field_rejected",
        "invalid_json", "control_operation_not_authorized", "control_argument_not_authorized", "control_loop_relaxes_limit",
        "control_source_not_approved", "control_base_loop_required", "control_loop_semantic_scope_required", "control_failure_replan_not_authorized",
        "control_draft_limits_invalid", "control_duplicate_loop_scope", "gepa_component_not_authorized", "gepa_components_changed",
        "gepa_model_budget_exhausted"};
    node["error"] = codes.count(code) ? code : "gepa_proposal_or_evaluation_failed";
    if(code == "gepa_trial_budget_exhausted" || code == "gepa_search_time_reserved") tree["stop_reason"] = code;
    if(code == "gepa_model_budget_exhausted") tree["reflection_exhausted"] = true;
    if(code == "gepa_model_not_configured" || code == "gepa_model_key_missing") {
      tree["status"] = "error"; tree["active"] = nullptr; save(); throw Error(code.c_str());
    }
    save();
  }
  void validate(std::size_t index) {
    try {
      const auto candidate = artifact(index);
      auto& node = tree["nodes"].items()[index];
      // An unchanged rule keeps evidence already established by either parent.
      // A changed rule has a new rule_id and therefore must be observed here.
      Json inherited{{"rules", Json::Object{}}};
      for(const auto parent : gepa::parents(node)) {
        const auto& parent_node = static_cast<const Json&>(tree)["nodes"].items()[parent];
        if(!parent_node["control_coverage"]["rules"].object()) continue;
        for(const auto& rule : parent_node["control_coverage"]["rules"].fields()) {
          auto& counts = inherited["rules"][rule.first];
          for(const auto* field : {"queries", "triggered", "applied"})
            counts[field] = std::max(counts.num(field, 0), rule.second.num(field, 0));
        }
      }
      const auto& screening = static_cast<const Json&>(node)["screening"]["control_coverage"];
      const auto training = merge_control_coverage(screening.object() ? screening : Json{{"rules", Json::Object{}}}, inherited);
      const auto probe_size = std::min(val.size(), config.num("minibatch_size", 4));
      if(!training.flag("complete", true) && probe_size < val.size() && config.flag("cache_evaluations", true)) {
        // Give controls missed by training a validation opportunity. Cached
        // trials are reused by full validation if the rule is observed here.
        const std::vector<std::string> probe(val.begin(), val.begin() + probe_size);
        const auto report = evaluate(candidate, probe, "val");
        const auto coverage = merge_control_coverage(control_coverage(candidate, report), inherited);
        const auto values = scores(report);
        node["validation_probe"] = Json{{"scores", values}, {"control_coverage", coverage}};
        for(const auto& rule : coverage["rules"].fields()) {
          const auto& previous = training["rules"][rule.first];
          if(!values.fields().empty() && previous.num("queries", 0) && !previous.num("applied", 0)
              && rule.second.num("queries", 0) && !rule.second.num("applied", 0)) {
            node["control_coverage"] = coverage;
            node["status"] = "unobserved_controls";
            node["reason"] = "control_unobserved_in_train_and_validation"; save(); return;
          }
        }
        save();
      }
      const auto report = evaluate(candidate, val, "val"); auto values = scores(report);
      node["validation"] = values;
      node["validation_applied_changes"] = Json::Object{};
      for(const auto parent : gepa::parents(node))
        node["validation_applied_changes"][std::to_string(parent)] = applied_changes(artifact(parent), report);
      if(!values.fields().empty()) node["optimization"] = annotate_validation(node["validation"]);
      node["validation_control_coverage"] = control_coverage(candidate, report);
      node["control_coverage"] = merge_control_coverage(node["validation_control_coverage"], training);
      bool changed = node["screening"]["applied_changes"].object() && !node["screening"]["applied_changes"].fields().empty();
      for(const auto& parent : node["validation_applied_changes"].fields()) changed |= !parent.second.fields().empty();
      node["status"] = values.fields().empty() ? "inconclusive"
          : !node["control_coverage"].flag("complete", false) ? "unobserved_controls"
          : changed ? "accepted" : "unobserved_changes";
      if(node.str("status") == "unobserved_changes") node["reason"] = "no_observed_decision_change";
      save();
    } catch(const std::exception& exception) { error(index, exception); }
  }
  void update_frontier(std::size_t previous_best, const std::string& previous_frontier) {
    Json ids = Json::Array{}; for(auto i : frontier()) ids.push(i);
    tree["stagnation"] = winner() == previous_best && ids.dump() == previous_frontier ? tree.num("stagnation", 0) + 1 : 0;
    tree["frontier"] = ids;
    tree["frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config); save();
  }
  void finish_batch() {
    auto& batch = tree["batch"];
    if(!batch.has("selected")) {
      batch["selected"] = Json::Array{};
      const auto selected = gepa::select_proposals(tree["nodes"], batch["nodes"], config);
      std::set<std::size_t> kept(selected.begin(), selected.end());
      for(auto index : selected) batch["selected"].push(index);
      for(const auto& id : batch["nodes"].items()) {
        auto& node = tree["nodes"].items()[id.integer()];
        if(node.str("status") == "screened" && !kept.count(id.integer())) node["status"] = "not_selected";
      }
      save();
    }
    for(const auto& id : batch["selected"].items()) {
      auto& node = tree["nodes"].items()[id.integer()];
      if(node.str("status") != "screened") continue;
      if(tree.str("stop_reason") == "gepa_trial_budget_exhausted" || tree.str("stop_reason") == "gepa_search_time_reserved") {
        node["status"] = "skipped_budget"; continue;
      }
      validate(id.integer());
      if(node.str("status") == "accepted") tree["merge_due"] = true;
    }
    const auto previous = batch.num("previous_best", 0); const auto old = batch.str("previous_frontier");
    tree["batch"] = nullptr; update_frontier(previous, old);
  }
  std::vector<std::string> merge_batch(std::size_t first, std::size_t second) const {
    std::vector<std::string> buckets[3], result;
    const auto& a = tree["nodes"].items()[first]["validation"];
    const auto& b = tree["nodes"].items()[second]["validation"];
    for(const auto& task : val) {
      require(a.has(task) && b.has(task), "gepa_merge_validation_missing");
      const auto difference = number(a[task]["fitness"]) - number(b[task]["fitness"]);
      buckets[difference > 1e-12 ? 0 : difference < -1e-12 ? 1 : 2].push_back(task);
    }
    std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + tree.num("rounds", 0)));
    for(auto& bucket : buckets) std::shuffle(bucket.begin(), bucket.end(), random);
    const auto count = std::min(val.size(), config.num("merge_minibatch_size", 4));
    while(result.size() < count) for(auto& bucket : buckets)
      if(!bucket.empty() && result.size() < count) { result.push_back(bucket.back()); bucket.pop_back(); }
    std::sort(result.begin(), result.end()); return result;
  }
  void merge(std::size_t index) {
    auto& node = tree["nodes"].items()[index]; const auto ids = gepa::parents(node);
    if(node.str("status") == "pending") {
      const auto a = artifact(ids[0]), b = artifact(ids[1]), base = artifact(node.num("ancestor", 0));
      const auto combined = gepa::recombine(components(base), components(a), components(b), ids[0], ids[1],
          !improves(tree["nodes"].items()[ids[1]]["validation"], tree["nodes"].items()[ids[0]]["validation"]));
      node["component_sources"] = combined["sources"]; node["conflicts"] = combined["conflicts"];
      if(!combined.flag("useful", false)) { node["status"] = "skipped_merge"; node["reason"] = "no_complementary_components"; save(); return; }
      proposal(index, compose(a, combined["components"]));
    }
    if(node.str("status") == "proposed") {
      std::vector<std::string> tasks; for(const auto& id : node["tasks"].items()) tasks.push_back(id.text());
      const auto report = evaluate(artifact(index), tasks, "val"); const auto values = scores(report);
      Json first = Json::Object{}, second = Json::Object{};
      for(const auto& id : tasks) {
        first[id] = tree["nodes"].items()[ids[0]]["validation"][id];
        second[id] = tree["nodes"].items()[ids[1]]["validation"][id];
      }
      node["screening"] = Json{{"parents", Json::Array{first, second}}, {"candidate", values}, {"split", "val"}};
      const auto changes_a = applied_changes(artifact(ids[0]), report), changes_b = applied_changes(artifact(ids[1]), report);
      node["screening"]["parent_applied_changes"] = Json::Array{changes_a, changes_b};
      const auto retained = [&](const Json& parent, const Json& changes) {
        return gepa::no_worse(values, parent) || (config.str("acceptance") == "pareto"
            && observed_local_gain(values, parent, changes));
      };
      node["status"] = values.fields().empty() ? "inconclusive"
          : retained(first, changes_a) && retained(second, changes_b) ? "screened" : "rejected_merge";
      save();
    }
    if(node.str("status") == "screened") validate(index);
  }
  GepaResult run() {
    dataset = read_evaluation_dataset(dataset_path);
    for(const auto& issue : dataset.issues()) require(issue.severity() != "error", "gepa_dataset_invalid");
    for(const auto& task : dataset.tasks()) {
      require(task.review_status() == "reviewed", "gepa_dataset_review_required");
      if(task.split() == "train") train.push_back(task.task_id());
      else if(task.split() == "val") val.push_back(task.task_id());
      else if(task.split() == "holdout") holdout.push_back(task.task_id());
    }
    require(!train.empty() && !val.empty() && !holdout.empty(), "gepa_three_splits_required");
    require(!source_experiences(seed).items().empty(), "gepa_approved_experiences_required");
    reserve = holdout.size() * evaluation.num("repetitions", 1) * 2;
    require(config.num("max_trials", 400) >= reserve + val.size() * evaluation.num("repetitions", 1) * 2,
        "gepa_holdout_budget_required");
    if(fs::exists(root / "tree.json")) {
      tree = Json::parse(read(root / "tree.json", 16777216));
      require((tree.num("schema_version", 0) == 1 || tree.num("schema_version", 0) == 2) && tree.str("context") == context
          && tree.str("seed_version") == agent_version_id(seed.dump())
          && tree.str("baseline_version") == agent_version_id(baseline.dump()), "gepa_resume_context_changed");
      require(tree["nodes"].array() && !tree["nodes"].items().empty()
          && tree["nodes"].items().size() <= config.num("max_rounds", 8) + 1, "gepa_tree_invalid");
      gepa::validate_nodes(tree["nodes"]);
      require(tree.num("rounds", SIZE_MAX) == tree["nodes"].items().size() - 1
          && tree.num("model_calls", SIZE_MAX) <= config.num("max_model_calls", 8)
          && tree.num("trials_used", SIZE_MAX) <= config.num("max_trials", 400), "gepa_tree_invalid");
      if(!tree["active"].null()) require(tree["active"].integer() > 0
          && tree["active"].integer() < tree["nodes"].items().size(), "gepa_tree_invalid");
      if(tree.has("batch") && !tree["batch"].null()) {
        const auto& batch = tree["batch"]; std::set<std::size_t> ids;
        require(batch.object() && batch["nodes"].array() && batch.num("limit", 0) > 0
            && batch.num("limit", 0) <= 8 && batch["nodes"].items().size() <= batch.num("limit", 0), "gepa_tree_invalid");
        for(const auto& id : batch["nodes"].items()) require(id.integer() > 0
            && id.integer() < tree["nodes"].items().size() && ids.insert(id.integer()).second
            && tree["nodes"].items()[id.integer()].str("kind") == "reflection", "gepa_tree_invalid");
        if(batch.has("plans")) {
          require(batch["plans"].array() && batch["plans"].items().size() == batch.num("limit", 0), "gepa_tree_invalid");
          for(const auto& plan : batch["plans"].items()) {
            require(plan.num("parent", SIZE_MAX) < tree["nodes"].items().size()
                && tree["nodes"].items()[plan.num("parent", 0)].str("status") == "accepted"
                && plan["tasks"].array() && !plan["tasks"].items().empty() && plan["components"].array(), "gepa_tree_invalid");
            for(const auto& task : plan["tasks"].items()) require(std::find(train.begin(), train.end(), task.text()) != train.end(), "gepa_tree_invalid");
          }
        }
        if(batch.has("selected")) for(const auto& id : batch["selected"].items()) require(ids.count(id.integer()), "gepa_tree_invalid");
      }
      for(auto& node : tree["nodes"].items()) if(node.str("status") == "reflecting") {
        node["status"] = "interrupted"; node["reason"] = "reflection_outcome_unknown"; tree["active"] = nullptr;
      }
    } else {
      tree = Json{{"schema_version", 2}, {"algorithm", "gepa-reflective-dag-v4-constrained-portfolio"}, {"context", context},
          {"seed_version", agent_version_id(seed.dump())}, {"baseline_version", agent_version_id(baseline.dump())},
          {"dataset_version", dataset.dataset_version()}, {"started_ms", now()}, {"rounds", 0}, {"model_calls", 0},
          {"trials_used", 0}, {"component_cursor", 0}, {"active", nullptr}, {"evaluations", Json::Object{}}, {"nodes", Json::Array{
            Json{{"id", 0}, {"parent", nullptr}, {"parents", Json::Array{}}, {"kind", "seed"}, {"depth", 0},
                {"version", agent_version_id(seed.dump())}, {"status", "seed"}, {"selections", 0}}}}};
      write(root / "versions/0.json", seed.dump()); save();
    }
    if(!tree.has("merge_attempts")) tree["merge_attempts"] = Json::Array{};
    if(!tree.has("batch")) tree["batch"] = nullptr;
    if(!tree.has("component_cursor")) tree["component_cursor"] = 0;
    if(!tree["component_selections"].object()) tree["component_selections"] = Json::Object{};
    if(!tree["training_opportunities"].object()) tree["training_opportunities"] = Json::Object{};
    tree["schema_version"] = 2; tree["algorithm"] = "gepa-reflective-dag-v4-constrained-portfolio";
    tree["optimization_contract"] = gate;
    tree["available_components"] = Json::Array{};
    const auto available = components(seed);
    for(const auto& item : available.fields()) tree["available_components"].push(item.first);
    tree["search_warnings"] = Json::Array{};
    tree["fixed_controls"] = Json::Array{};
    if(proto<v1::RuntimePolicy>(seed["policy"]).options().requested_level() >= v1::L2)
      tree["fixed_controls"].push("observed_tool_required_arguments");
    if(tree["nodes"].items()[0].str("status") == "seed") {
      auto& root_node = tree["nodes"].items()[0];
      if(!root_node["validation"].object() || root_node["validation"].fields().empty()) {
        const auto report = evaluate(seed, val, "val"); const auto values = scores(report);
        require(!values.fields().empty(), "gepa_seed_evaluation_inconclusive");
        root_node["validation"] = values;
        root_node["control_coverage"] = control_coverage(seed, report);
        save();
      }
    }
    // Validate the deployable seed before spending an additional fixed pass on
    // the L0 reference. Validation content remains outside every model packet.
    if(!tree.has("baseline_validation")) {
      Json task_ids = Json::Array{};
      for(const auto& id : val) task_ids.push(id);
      const Json identity{{"schema_version", 1}, {"evaluation_context", evaluation_context},
          {"deterministic_contracts", "schedule-success-v1"},
          {"baseline_version", agent_version_id(baseline.dump())}, {"dataset_version", dataset.dataset_version()},
          {"task_ids", task_ids}, {"objective", config["objective"]},
          {"unknown_token_policy", config.str("unknown_token_policy", "reject")}, {"gate", gate}};
      const auto key = hash(identity.dump());
      const auto cache_path = root.parent_path().parent_path() / "baseline-cache" / (key + ".json");
      Json values; bool hit = false;
      bool imported = false;
      if(config.has("baseline_report_file")) {
        const auto bytes = read(config.str("baseline_report_file"), 134217728);
        const auto report = proto<Report>(Json::parse(bytes));
        const auto source = json(report.configuration());
        auto judge_path = fs::path(evaluation["judge"].str("runtime_config"));
        if(judge_path.is_relative()) judge_path = evaluation_path.parent_path() / judge_path;
        const auto judge = legacy::load_runtime_configuration(judge_path);
        require(report.complete() && report.dataset_version() == dataset.dataset_version()
            && report.evaluator_version() == "task-evaluator-v1"
            && source["variants"].array() && source["variants"].items().size() == 1
            && source["variants"].items().front().str("version") == agent_version_id(baseline.dump())
            && source["splits"].array() && source["splits"].items().size() == 1
            && source["splits"].items().front().text() == "val"
            && source.str("judge_model") == judge.model.model_id
            && source.flag("model_enabled", false) == evaluation["judge"].flag("enabled", false),
            "gepa_baseline_report_incompatible");
        std::set<std::string> observed;
        for(const auto& trial : report.trials())
          require(trial.variant_id() == "candidate" && trial.split() == "val"
              && trial.repetition() == 1 && observed.insert(trial.task_id()).second,
              "gepa_baseline_report_incompatible");
        values = scores(report);
        require(observed.size() == val.size() && values.fields().size() == val.size(),
            "gepa_baseline_report_incompatible");
        for(const auto& id : val) require(observed.count(id) && values.has(id), "gepa_baseline_report_incompatible");
        hit = imported = true;
      }
      if(!imported && fs::exists(cache_path)) try {
        const auto cached = Json::parse(read(cache_path, 1048576));
        const auto values_bytes = cached.str("values_json");
        values = Json::parse(values_bytes);
        hit = cached["identity"].dump() == identity.dump() && values.object()
            && values.fields().size() == val.size() && cached.str("values_hash") == hash(values_bytes);
        for(const auto& id : val) hit &= values.has(id) && values[id].object()
            && values[id].has("fitness") && values[id].has("quality") && values[id].has("duration_ms");
      } catch(const std::exception&) { hit = false; }
      tree["baseline_cache"] = Json{{"key", key}, {"status", imported ? "imported" : hit ? "hit" : "miss"}};
      if(!hit) {
        const auto report = evaluate(baseline, val, "val"); values = scores(report);
        require(values.fields().size() == val.size(), "gepa_baseline_evaluation_inconclusive");
      }
      tree["baseline_validation"] = values; save();
      if(!hit) try {
        const auto values_bytes = values.dump();
        write(cache_path, Json{{"identity", identity}, {"values_json", values_bytes}, {"values_hash", hash(values_bytes)},
            {"created_ms", now()}}.dump());
      } catch(const std::exception&) {
        tree["baseline_cache"]["status"] = "write_failed"; save();
      }
    }
    if(tree["nodes"].items()[0].str("status") == "seed") {
      auto& root_node = tree["nodes"].items()[0];
      root_node["optimization"] = annotate_validation(root_node["validation"]);
      root_node["status"] = "accepted"; save();
    } else if(!tree["nodes"].items()[0]["optimization"].object()) {
      tree["nodes"].items()[0]["optimization"] = annotate_validation(tree["nodes"].items()[0]["validation"]);
      save();
    }
    if(!tree.has("frontier") || tree["frontier"].null()) {
      tree["frontier"] = Json::Array{}; for(auto id : frontier()) tree["frontier"].push(id);
      tree["frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config);
    }
    tree["status"] = "searching";
    tree.fields().erase("error"); tree.fields().erase("finished_ms"); save();
    while(!tree.flag("holdout_started", false) && (!tree["active"].null() || !tree["batch"].null()
        || tree.num("rounds", 0) < config.num("max_rounds", 8))) {
      check();
      if(now() >= deadline() - holdout_time() || tree.str("stop_reason") == "gepa_search_time_reserved") {
        tree["stop_reason"] = "gepa_search_time_reserved";
        if(!tree["active"].null()) tree["nodes"].items()[tree["active"].integer()]["status"] = "skipped_budget";
        tree["active"] = nullptr;
        if(!tree["batch"].null()) finish_batch();
        save(); break;
      }
      if(tree["active"].null() && !tree["batch"].null()
          && (tree["batch"].has("selected") || tree["batch"]["nodes"].items().size() >= tree["batch"].num("limit", 1)
              || tree.num("model_calls", 0) >= config.num("max_model_calls", 8)
              || tree.flag("reflection_exhausted", false)
              || tree.str("stop_reason") == "gepa_trial_budget_exhausted")) {
        finish_batch();
        if(tree.str("stop_reason") == "gepa_trial_budget_exhausted") break;
        continue;
      }
      if(tree["active"].null() && tree["batch"].null()) {
        if(config.has("target_fitness") && mean(tree["nodes"].items()[winner()]["validation"], "quality") >= 1
            && mean(tree["nodes"].items()[winner()]["validation"], "fitness") >= number(config["target_fitness"], 1)) {
          tree["stop_reason"] = "target_fitness"; break;
        }
        if(config.num("patience", 0) && tree.num("stagnation", 0) >= config.num("patience", 0)) { tree["stop_reason"] = "patience"; break; }
        if(config.flag("use_merge", true) && tree.flag("merge_due", false)
            && tree["merge_attempts"].items().size() < config.num("max_merge_invocations", 5)) {
          const auto plan = gepa::select_merge(tree["nodes"], gepa::frontier_map(tree["nodes"], val, config),
              tree["merge_attempts"], config, tree.num("rounds", 0));
          tree["merge_due"] = false;
          if(!plan.null()) {
            const auto a = plan.num("first", 0), b = plan.num("second", 0), index = tree["nodes"].items().size();
            Json node{{"id", index}, {"parent", a}, {"parents", Json::Array{a, b}}, {"ancestor", plan["ancestor"]},
                {"kind", "merge"}, {"status", "pending"}, {"selections", 0}, {"previous_best", winner()},
                {"component_cursor", std::max(tree["nodes"].items()[a].num("component_cursor", 0), tree["nodes"].items()[b].num("component_cursor", 0))},
                {"previous_frontier", tree["frontier"].dump()}, {"depth", std::max(tree["nodes"].items()[a].num("depth", 0), tree["nodes"].items()[b].num("depth", 0)) + 1},
                {"tasks", Json::Array{}}};
            for(const auto& id : merge_batch(a, b)) node["tasks"].push(id);
            for(auto p : {a, b}) tree["nodes"].items()[p]["selections"] = tree["nodes"].items()[p].num("selections", 0) + 1;
            auto attempt = plan; attempt["node"] = index; tree["merge_attempts"].push(attempt);
            tree["nodes"].push(node); tree["active"] = index; tree["rounds"] = tree.num("rounds", 0) + 1; save();
          }
        }
        if(tree["active"].null()) {
          if(tree.flag("reflection_exhausted", false) || tree.num("model_calls", 0) >= config.num("max_model_calls", 8)) { tree["stop_reason"] = "model_budget"; break; }
          const auto sequence = tree.num("training_batches", tree.num("reflection_batches", 0));
          const auto sampling = config.str("sampling_strategy", "same_parent");
          const auto n = sampling == "single" ? 1 : config.num("proposals_per_round", 1);
          const auto total = n * (sampling == "pxn" ? config.num("parents_per_round", 2) : 1);
          const auto count = std::min({total, config.num("max_rounds", 8) - tree.num("rounds", 0),
              config.num("max_model_calls", 8) - tree.num("model_calls", 0)});
          tree["batch"] = Json{{"id", tree.num("reflection_batches", 0)}, {"limit", count}, {"nodes", Json::Array{}}, {"plans", Json::Array{}},
              {"previous_best", winner()}, {"previous_frontier", tree["frontier"].dump()}};
          std::size_t groups = 0;
          while(tree["batch"]["plans"].items().size() < count) {
            const auto p = gepa::select_parent(tree["nodes"], gepa::frontier_map(tree["nodes"], val, config), config, tree.num("rounds", 0) + groups);
            const auto cursor = tree.num("component_cursor", 0);
            Json plan{{"parent", p}, {"tasks", Json::Array{}},
                {"components", gepa::select_components(components(artifact(p)), config.str("component_selector", "round_robin"), cursor)},
                {"component_slot", cursor}, {"component_cursor", cursor + 1}};
            for(const auto& id : component_batch(plan["components"], sequence + groups)) plan["tasks"].push(id);
            tree["component_cursor"] = cursor + 1; ++groups;
            const auto repeats = sampling == "independent" ? 1 : n;
            for(std::size_t i = 0; i < repeats && tree["batch"]["plans"].items().size() < count; ++i) tree["batch"]["plans"].push(plan);
          }
          tree["training_batches"] = sequence + groups;
          tree["reflection_batches"] = tree.num("reflection_batches", 0) + 1; save();
        }
      }
      if(tree["active"].null()) {
        const auto& plan = tree["batch"].has("plans")
            ? tree["batch"]["plans"].items().at(tree["batch"]["nodes"].items().size()) : tree["batch"];
        const auto p = plan.num("parent", 0), index = tree["nodes"].items().size();
        tree["nodes"].items()[p]["selections"] = tree["nodes"].items()[p].num("selections", 0) + 1;
        tree["nodes"].push(Json{{"id", index}, {"parent", p}, {"parents", Json::Array{p}}, {"kind", "reflection"},
            {"depth", tree["nodes"].items()[p].num("depth", 0) + 1}, {"status", "pending"}, {"selections", 0},
            {"component_slot", plan.num("component_slot", 0)},
            {"component_cursor", plan.num("component_cursor", tree["nodes"].items()[p].num("component_cursor", 0))},
            {"batch", tree["batch"]["id"]}, {"components", plan["components"]}, {"tasks", plan["tasks"]}});
        tree["batch"]["nodes"].push(index);
        tree["active"] = index; tree["rounds"] = tree.num("rounds", 0) + 1; save();
      }
      const auto index = tree["active"].integer(); auto& node = tree["nodes"].items()[index];
      try {
        if(node.str("kind") == "merge") {
          merge(index);
        } else {
          const auto p = node.num("parent", 0); const auto parent_artifact = artifact(p);
          std::vector<std::string> batch;
          if(node.has("tasks")) for(const auto& id : node["tasks"].items()) batch.push_back(id.text());
          else batch = minibatch(index - 1);
          const auto parent_report = evaluate(parent_artifact, batch, "train"); const auto parent_scores = scores(parent_report);
          require(!parent_scores.fields().empty(), "gepa_parent_evaluation_inconclusive");
          remember_opportunities(parent_report);
          if(node.str("status") == "pending") {
            bool perfect = config.flag("skip_perfect_score", false);
            for(const auto& item : parent_scores.fields()) perfect &= number(item.second["quality"]) >= 1
                && number(item.second["fitness"]) >= number(config["perfect_score"], 1);
            if(perfect) { node["status"] = "skipped_perfect"; save(); }
            else {
              auto selected = node.has("components") ? node["components"] : gepa::select_components(components(parent_artifact), "all", 0);
              const auto available = components(parent_artifact);
              const auto observed = applied_experiences(parent_report);
              auto input = packet(parent_artifact, parent_report, selected);
              if(config.str("component_selector", "round_robin") == "round_robin" && selected.items().size() == 1) {
                // Give a newly observed control opportunity a turn before another
                // experience rewrite; this does not require it to win the search.
                for(const auto* component : {"schedule_rules", "action_rules", "loop_rules"}) {
                  if(!available.has(component) || tree["component_selections"].num(component, 0)) continue;
                  const Json option = Json::Array{component};
                  auto probe = packet(parent_artifact, parent_report, option);
                  if(!control_evidence(probe, component)) continue;
                  if(selected.items().front().text() != component)
                    node["component_fallback"] = Json{{"from", selected.items().front()}, {"to", component}, {"reason", "unexplored_control_opportunity"}};
                  selected = option; node["components"] = selected; input = std::move(probe); break;
                }
              }
              if(config.str("component_selector", "round_robin") == "round_robin"
                  && selected.items().size() == 1
                  && (selected.items().front().text() == "action_rules" || selected.items().front().text() == "loop_rules"
                      || selected.items().front().text() == "schedule_rules")
                  && !control_evidence(input, selected.items().front().text())) {
                const auto skipped = selected.items().front().text();
                tree["deferred_components"][skipped] = tree["deferred_components"].num(skipped, 0) + 1;
                const auto slot = node.num("component_slot", 0);
                auto fallback = observed_experience(available, observed);
                if(fallback.items().empty()) for(std::size_t offset = 1; offset <= available.fields().size() * 3; ++offset) {
                  const auto candidate = gepa::select_components(available, "round_robin", slot + offset);
                  if(candidate.items().front().text() == "action_rules" || candidate.items().front().text() == "loop_rules"
                      || candidate.items().front().text() == "schedule_rules") continue;
                  fallback = candidate; break;
                }
                if(!fallback.items().empty()) {
                  selected = fallback; node["components"] = selected;
                  node["component_fallback"] = Json{{"from", skipped}, {"to", selected.items().front()},
                      {"reason", "no_executable_training_evidence"}};
                  input = packet(parent_artifact, parent_report, selected); save();
                }
              }
              if(config.str("component_selector", "round_robin") == "round_robin"
                  && selected.items().size() == 1 && selected.items().front().text().rfind("experience:", 0) == 0
                  && !observed.empty() && !observed.count(selected.items().front().text())) {
                const auto fallback = observed_experience(available, observed);
                if(!fallback.items().empty()) {
                  const auto skipped = selected.items().front(); selected = fallback; node["components"] = selected;
                  node["component_fallback"] = Json{{"from", skipped}, {"to", selected.items().front()},
                      {"reason", "experience_not_applied_in_training"}};
                  input = packet(parent_artifact, parent_report, selected); save();
                }
              }
              if(!node.flag("component_reserved", false)) {
                for(const auto& component : selected.items()) tree["component_selections"][component.text()]
                    = tree["component_selections"].num(component.text(), 0) + 1;
                node["component_reserved"] = true; save();
              }
              const auto draft = propose(parent_artifact, input, index);
              write(root / "reflections" / (std::to_string(index) + ".draft.json"), draft.dump());
              Json candidate, accepted = draft;
              Json contract_repairs;
              accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
              if(!contract_repairs.items().empty()) node["schedule_contract_repair"] = contract_repairs;
              try { candidate = mutate(parent_artifact, accepted, selected); }
              catch(const std::exception& exception) {
                const std::string code = exception.what();
                if((code != "control_source_not_approved" && code != "gepa_component_not_authorized"
                        && code != "control_failure_replan_not_authorized")
                    || controls.num("max_attempts", 2) < 2
                    || tree.num("model_calls", 0) >= config.num("max_model_calls", 8)) throw;
                // Repair once within the same model and input budgets. Source
                // correction needs no traces; component correction needs evidence.
                Json repair{{"phase", code == "control_source_not_approved"
                        ? "repair_control_source" : "repair_component_selection"},
                    {"error", code}, {"draft", draft}, {"experiences", input["experiences"]},
                    {"selected_components", selected}};
                if(input.has("controls")) repair["controls"] = input["controls"];
                if(code == "gepa_component_not_authorized" || code == "control_failure_replan_not_authorized") {
                  repair = input;
                  repair.fields().erase("parent_version");
                  repair["phase"] = code == "gepa_component_not_authorized"
                      ? "repair_component_selection" : "repair_control_capability";
                  repair["error"] = code;
                  repair["feedback"] = Json::Array{};
                  const Json* evidence = nullptr;
                  for(const auto& row : input["feedback"].items()) {
                    if(!evidence) evidence = &row;
                    const Json single{{"feedback", Json::Array{row}}};
                    if(std::any_of(selected.items().begin(), selected.items().end(),
                        [&](const auto& component) { return control_evidence(single, component.text()); })) {
                      evidence = &row; break;
                    }
                  }
                  // Reuse one relevant training row, not the unauthorized draft
                  // or the whole minibatch. Keep its task contract and trace intact.
                  if(evidence) repair["feedback"].push(*evidence);
                }
                node["repair_error"] = code; save();
                accepted = reflect(repair, index, "-repair");
                write(root / "reflections" / (std::to_string(index) + "-repair.draft.json"), accepted.dump());
                accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
                if(!contract_repairs.items().empty()) node["schedule_contract_repair"] = contract_repairs;
                candidate = mutate(parent_artifact, accepted, selected);
              }
              node["reason"] = accepted.str("reason");
              node["control_preflight"] = control_preflight(parent_artifact, candidate, parent_report);
              if(!node["control_preflight"].flag("passed", false)
                  && std::any_of(selected.items().begin(), selected.items().end(), [](const auto& item) {
                    return item.text() == "schedule_rules";
                  })) {
                Json repairs;
                auto repaired_draft = repair_schedule_guards(accepted, parent_report, repairs);
                if(!repairs.items().empty()) {
                  node["schedule_guard_repair"] = repairs;
                  auto repaired = mutate(parent_artifact, repaired_draft, selected);
                  auto check = control_preflight(parent_artifact, repaired, parent_report);
                  if(check.flag("passed", false)) {
                    candidate = std::move(repaired); accepted = std::move(repaired_draft);
                    node["control_preflight"] = std::move(check);
                  }
                }
              }
              if(!node["control_preflight"].flag("passed", false)
                  && config.str("component_selector", "round_robin") == "round_robin") {
                node["status"] = "untriggerable_controls";
                node["reason"] = "no_differential_training_trigger"; save();
              } else proposal(index, candidate);
            }
          }
          if(node.str("status") == "proposed") {
            const auto child = artifact(index); const auto screening = evaluate(child, batch, "train"); const auto values = scores(screening);
            node["screening"] = Json{{"parent", parent_scores}, {"candidate", values}};
            node["screening"]["control_coverage"] = control_coverage(child, screening);
            node["screening"]["applied_changes"] = applied_changes(parent_artifact, screening);
            const auto acceptance = config.str("acceptance", "strict_improvement");
            const bool pass = acceptance == "pareto"
                ? gepa::no_worse(values, parent_scores) || observed_local_gain(values, parent_scores,
                    node["screening"]["applied_changes"])
                : acceptance == "improvement_or_equal" ? gepa::no_worse(values, parent_scores) : improves(values, parent_scores);
            node["status"] = values.fields().empty() ? "inconclusive" : pass ? "screened" : "rejected_minibatch"; save();
            // Validation, not the small training screen, decides control coverage.
          }
          if(node.str("status") == "screened" && tree["batch"].null()) validate(index);
        }
      } catch(const std::exception& exception) { error(index, exception); }
      tree["active"] = nullptr;
      if(node.str("kind") == "merge" || tree["batch"].null()) {
        tree["merge_due"] = node.str("status") == "accepted";
        update_frontier(node.num("previous_best", 0), node.str("previous_frontier"));
      }
      save();
      if((tree.str("stop_reason") == "gepa_trial_budget_exhausted" || tree.str("stop_reason") == "gepa_search_time_reserved")
          && tree["batch"].null()) break;
    }
    if(!tree.flag("holdout_started", false)) {
      tree["winner"] = winner(); tree["holdout_started"] = true;
      if(!tree.has("stop_reason")) tree["stop_reason"] = tree.flag("reflection_exhausted", false)
          || tree.num("model_calls", 0) >= config.num("max_model_calls", 8) ? "model_budget" : "max_rounds";
      save();
    }
    const auto selected = artifact(tree.num("winner", 0));
    const auto report = evaluate(selected, holdout, "holdout", true);
    tree["status"] = scored(report) ? "completed" : "inconclusive"; tree["finished_ms"] = now();
    tree["selected_version"] = agent_version_id(selected.dump()); save();
    // Selection is not publication: the caller still applies the deployment gate.
    write(root / "selected-version.json", selected.dump());
    Json summary{{"algorithm", tree["algorithm"]}, {"status", tree["status"]}, {"rounds", tree["rounds"]},
        {"nodes", tree["nodes"].items().size()}, {"model_calls", tree["model_calls"]}, {"trials_used", tree["trials_used"]},
        {"winner", tree["winner"]}, {"selected_version", tree["selected_version"]}, {"stop_reason", tree["stop_reason"]}};
    summary["merge_attempts"] = tree["merge_attempts"].items().size();
    summary["reflection_batches"] = tree.num("reflection_batches", 0);
    summary["available_components"] = tree["available_components"];
    summary["search_warnings"] = tree["search_warnings"];
    summary["fixed_controls"] = tree["fixed_controls"];
    summary["control_coverage"] = tree["nodes"].items()[tree.num("winner", 0)]["control_coverage"];
    summary["optimization"] = tree["nodes"].items()[tree.num("winner", 0)]["optimization"];
    summary["component_selections"] = tree["component_selections"];
    summary["training_opportunities"] = tree["training_opportunities"];
    summary["deferred_components"] = tree["deferred_components"];
    return {selected, report, summary};
  }
};
}  // namespace

Json load_gepa_config(const fs::path& path) {
  auto config = Json::parse(read(path));
  keys(config, {"schema_version", "runtime_config", "prompt_file", "max_rounds", "max_model_calls", "max_trials", "timeout_ms", "holdout_reserve_ms",
      "minibatch_size", "max_feedback_trials", "max_input_bytes", "max_experience_bytes", "patience", "seed", "min_improvement", "objective",
      "use_merge", "max_merge_invocations", "merge_minibatch_size", "candidate_selection_strategy", "frontier_type", "prune_dominated",
      "epsilon", "top_k", "component_selector", "minibatch_strategy", "skip_perfect_score", "perfect_score", "acceptance",
      "proposals_per_round", "proposal_selection", "proposal_top_k", "cache_evaluations",
      "sampling_strategy", "parents_per_round", "target_fitness",
      "reflection_strategy", "duplication_factor", "aggregation_prompt_file", "unknown_token_policy", "baseline_report_file"});
  require(config.num("schema_version", 0) == 1, "gepa_config_invalid");
  bound(config, "max_rounds", 8, 1, 256); bound(config, "max_model_calls", 8, 1, 256);
  bound(config, "max_trials", 400, 4, 10000); bound(config, "timeout_ms", 1800000, 1000, 86400000);
  bound(config, "holdout_reserve_ms", 0, 0, config.num("timeout_ms", 1800000) - 1);
  bound(config, "minibatch_size", 4, 1, 128); bound(config, "max_feedback_trials", 4, 1, 32);
  bound(config, "max_input_bytes", 65536, 8192, 1048576); bound(config, "max_experience_bytes", 2048, 64, 2048);
  bound(config, "patience", 0, 0, 256); bound(config, "seed", 42, 0, 2147483000);
  bound(config, "max_merge_invocations", 5, 0, 256); bound(config, "merge_minibatch_size", 4, 1, 128);
  bound(config, "top_k", 5, 1, 256); bound(config, "proposals_per_round", 1, 1, 8);
  bound(config, "proposal_top_k", 2, 1, 8);
  bound(config, "parents_per_round", 2, 1, 8);
  bound(config, "duplication_factor", 2, 1, 4);
  require(config.str("sampling_strategy", "same_parent") != "pxn"
      || config.num("proposals_per_round", 1) * config.num("parents_per_round", 2) <= 8, "gepa_batch_limit");
  for(const auto* key : {"use_merge", "prune_dominated", "skip_perfect_score", "cache_evaluations"}) if(config.has(key)) config[key].boolean();
  auto choice = [&](const char* key, const char* fallback, std::initializer_list<const char*> allowed) {
    const auto value = config.str(key, fallback);
    require(std::any_of(allowed.begin(), allowed.end(), [&](auto item) { return value == item; }), "gepa_strategy_invalid");
  };
  choice("candidate_selection_strategy", "pareto", {"pareto", "current_best", "epsilon_greedy", "top_k_pareto", "fair"});
  choice("frontier_type", "instance", {"instance", "objective", "hybrid", "cartesian"});
  choice("component_selector", "round_robin", {"round_robin", "all", "model_choice"});
  choice("minibatch_strategy", "epoch_shuffled", {"epoch_shuffled", "fixed"});
  choice("acceptance", "strict_improvement", {"strict_improvement", "improvement_or_equal", "pareto"});
  choice("proposal_selection", "all_improvements", {"all_improvements", "best", "top_k"});
  choice("sampling_strategy", "same_parent", {"single", "same_parent", "independent", "pxn"});
  choice("reflection_strategy", "single", {"single", "combee"});
  choice("unknown_token_policy", "reject", {"reject", "worst_case"});
  for(const auto* key : {"epsilon", "perfect_score", "target_fitness"}) if(config.has(key))
    require(number(config[key], -1) >= 0 && number(config[key], -1) <= 1, "gepa_strategy_invalid");
  require(number(config["min_improvement"], 0.000001) > 0 && number(config["min_improvement"], 0.000001) <= 1, "gepa_improvement_invalid");
  if(!config.has("objective")) config["objective"] = Json::Object{};
  keys(config["objective"], {"invalid_loop_weight", "token_weight", "latency_weight", "invalid_loop_scale", "token_scale", "latency_scale_ms"});
  double sum = 0;
  for(const auto& item : config["objective"].fields()) {
    const auto value = number(item.second, -1); require(value >= 0, "gepa_objective_invalid");
    if(item.first.find("weight") != std::string::npos) require(value <= 0.49, "gepa_objective_invalid");
    else require(value > 0, "gepa_objective_invalid");
  }
  sum = number(config["objective"]["invalid_loop_weight"], 0.2) + number(config["objective"]["token_weight"], 0.05) + number(config["objective"]["latency_weight"], 0);
  require(sum < 0.5, "gepa_quality_weight_required");
  for(const auto* key : {"runtime_config", "prompt_file"}) {
    fs::path value(config.str(key)); require(!value.empty(), "gepa_configuration_path_required");
    if(value.is_relative()) value = fs::absolute(path).parent_path() / value;
    config[key] = value.lexically_normal().string();
  }
  if(config.has("aggregation_prompt_file") || config.str("reflection_strategy", "single") == "combee") {
    fs::path value = config.has("aggregation_prompt_file") ? fs::path(config.str("aggregation_prompt_file"))
        : fs::path(config.str("prompt_file")).parent_path() / "gepa-aggregate.txt";
    require(!value.empty(), "gepa_configuration_path_required");
    if(value.is_relative()) value = fs::absolute(path).parent_path() / value;
    config["aggregation_prompt_file"] = value.lexically_normal().string();
  }
  if(config.has("baseline_report_file")) {
    fs::path value(config.str("baseline_report_file"));
    require(!value.empty(), "gepa_configuration_path_required");
    if(value.is_relative()) value = fs::absolute(path).parent_path() / value;
    config["baseline_report_file"] = value.lexically_normal().string();
  }
  config["config_file"] = fs::absolute(path).lexically_normal().string();
  validate_local_data(config); return config;
}

std::string gepa_config_fingerprint(const Json& config) {
  if(config.null()) return {};
  auto runtime = legacy::load_runtime_configuration(config.str("runtime_config"));
  std::string bytes = "gepa-reflective-dag-v4-constrained-portfolio:" + config.dump() + read(config.str("config_file")) + read(config.str("prompt_file"));
  if(config.has("aggregation_prompt_file")) bytes += read(config.str("aggregation_prompt_file"));
  for(const auto& path : runtime.source_files) bytes += read(path);
  if(runtime.model.endpoint.empty()) if(const auto* base = std::getenv("OPENAI_BASE_URL")) bytes += base;
  if(runtime.model.model_id.empty()) if(const auto* model = std::getenv("OPENAI_MODEL")) bytes += model;
  return hash(bytes);
}

GepaResult run_gepa_search(const Json& seed, const Json& baseline, const fs::path& dataset,
    const fs::path& evaluation_path, const Json& evaluation, const Json& config, const Json& gate,
    const Json& controls,
    const fs::path& output, const std::string& context, const std::string& evaluation_context,
    const std::function<bool()>& cancelled,
    std::shared_ptr<legacy::ModelAdapter> reflection_model) {
  Search search;
  search.seed = seed; search.baseline = baseline; search.dataset_path = dataset; search.evaluation_path = evaluation_path;
  search.evaluation = evaluation; search.config = config; search.gate = gate; search.controls = controls; search.root = output;
  search.context = context; search.evaluation_context = evaluation_context;
  search.cancelled = cancelled; search.model = std::move(reflection_model);
  try { return search.run(); }
  catch(const std::exception& exc) {
    if(search.tree.str("context") == context && search.tree["nodes"].array()) {
      const std::string code = exc.what();
      search.tree["status"] = code == "gepa_cancelled" ? "cancelled" : "error";
      const std::set<std::string> known{"gepa_cancelled", "gepa_time_budget_exhausted", "gepa_search_time_reserved",
          "gepa_model_rate_limited", "gepa_baseline_evaluation_inconclusive", "gepa_seed_evaluation_inconclusive",
          "gepa_model_not_configured", "gepa_model_key_missing"};
      search.tree["error"] = known.count(code) ? code : "gepa_search_failed";
      search.tree["finished_ms"] = now(); search.save();
    }
    throw;
  }
}
}  // namespace trajectory::evolution::integration
