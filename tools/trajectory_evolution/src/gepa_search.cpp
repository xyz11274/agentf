#include "gepa_search.h"
#include "gepa_strategy.h"
#include "control_generation.h"
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
Json source_experiences(const Json& artifact) {
  Json result = Json::Array{};
  const auto policy = proto<v1::RuntimePolicy>(artifact["policy"]);
  for(const auto& item : policy.snapshot().experiences()) {
    bool approved = false;
    for(const auto& ref : policy.options().approved_experiences())
      approved |= ref.memory_id() == item.memory_id() && ref.content_revision() == item.content_revision();
    if(!approved) continue;
    auto content = item.content();
    for(const auto& rewrite : policy.options().experience_overrides())
      if(rewrite.source_experience().memory_id() == item.memory_id()) content = rewrite.content();
    result.push(Json{{"memory_id", item.memory_id()}, {"scope", item.target_scope()},
        {"condition", item.trigger_pattern()}, {"content", content}});
  }
  return result;
}

Json feedback_trial(const dataset::v1::EvaluationTrial& trial) {
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
    std::map<std::string, Json> starts;
    for(const auto& event : trace["events"].items())
      if(event.str("kind") == "action.start") starts[event.str("action_id")] = event;
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

struct Search {
  fs::path dataset_path, evaluation_path, root;
  Json seed, baseline, evaluation, config, controls, tree;
  std::string context;
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
    const auto cost = ids.size() * evaluation.num("repetitions", 1) * (final ? 2 : 1);
    require(tree.num("trials_used", 0) + cost + (final ? 0 : reserve) <= config.num("max_trials", 400), "gepa_trial_budget_exhausted");
    tree["trials_used"] = tree.num("trials_used", 0) + cost;
    cache["attempt"] = cache.num("attempt", 0) + 1; cache["status"] = "running"; cache["trials_reserved"] = cost;
    cache["version"] = b["version"]; cache["split"] = split; cache["task_ids"] = selected;
    if(final) cache["baseline_version"] = a["version"];
    tree["evaluations"][key] = cache;
    save();
    TaskEvaluationOptions options; options.config_json = settings.dump();
    options.cancelled = [&] { return (cancelled && cancelled()) || now() >= tree.num("started_ms", 0) + config.num("timeout_ms", 1800000); };
    if(cache.num("attempt", 0) > 1 && (final || config.flag("cache_evaluations", true))) {
      const auto previous = root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0) - 1));
      // Resume validates context, manifest and artifacts, retaining execution
      // evidence while retrying only failed judgments or unfinished trials.
      if(fs::exists(previous / "manifest.json")) options.resume_report = previous / "report.json";
    }
    const auto directory = root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
    auto report = run_task_evaluation(dataset_path, evaluation_path, directory, options);
    check();
    cache["status"] = scored(report) && (final || scores(report).fields().size() == ids.size()) ? "complete" : "inconclusive";
    cache["report_hash"] = hash(read(directory / "report.json", 134217728));
    cache["report"] = (fs::path("evaluations") / key / directory.filename() / "report.json").string();
    tree["evaluations"][key] = cache;
    save();
    for(const auto& trial : report.trials()) for(const auto& check : trial.checks())
      if(check.check_id() == "model" && check.status() == "error" && check.reason() == "evaluation_judge_http_429")
        throw Error("gepa_model_rate_limited");
    return report;
  }
  Json scores(const Report& report) const {
    Json result = Json::Object{};
    if(!scored(report)) return result;
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto metrics = json(trial.metrics()), evidence = json(trial.evidence());
      const auto loops = evidence.has("loop_review")
          ? (evidence["loop_review"].str("status") == "complete" ? evidence["loop_review"]["count"] : Json())
          : evidence["trajectory"]["summary"]["invalid_loop_lower_bound"];
      const auto& objective = config["objective"];
      const auto lw = number(objective["invalid_loop_weight"], 0.2);
      const auto tw = number(objective["token_weight"], 0.05);
      const auto dw = number(objective["latency_weight"], 0);
      const bool unknown_tokens = metrics["tokens"].null();
      if((lw && loops.null()) || (tw && unknown_tokens
          && config.str("unknown_token_policy", "reject") != "worst_case")) return Json(Json::Object{});
      auto penalty = [](double x, double scale) { return x / (scale + x); };
      // This bounded cost has supremum 1. An explicit conservative policy
      // uses that bound for ranking; it never invents a measured token count.
      const auto token_cost = unknown_tokens ? 1.0
          : penalty(number(metrics["tokens"]), number(objective["token_scale"], 10000));
      const double fitness = (trial.verdict() == "passed" ? 1.0 : 0.0)
          - lw * penalty(number(loops), number(objective["invalid_loop_scale"], 1))
          - tw * token_cost
          - dw * penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      auto& row = result[trial.task_id()];
      row["fitness"] = number(row["fitness"]) + fitness;
      row["quality"] = number(row["quality"]) + (trial.verdict() == "passed" ? 1 : 0);
      if(lw) row["loop_efficiency"] = number(row["loop_efficiency"]) + 1 - penalty(number(loops), number(objective["invalid_loop_scale"], 1));
      if(tw) row["token_efficiency"] = number(row["token_efficiency"]) + 1 - token_cost;
      if(tw && unknown_tokens) row["unknown_token_trials"] = row.num("unknown_token_trials", 0) + 1;
      if(dw) row["latency_efficiency"] = number(row["latency_efficiency"]) + 1 - penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      row["repetitions"] = row.num("repetitions", 0) + 1;
    }
    for(auto& item : result.fields()) {
      item.second["fitness"] = number(item.second["fitness"]) / item.second.num("repetitions", 1);
      item.second["quality"] = number(item.second["quality"]) / item.second.num("repetitions", 1);
      for(const auto* field : {"loop_efficiency", "token_efficiency", "latency_efficiency"})
        if(item.second.has(field)) item.second[field] = number(item.second[field]) / item.second.num("repetitions", 1);
    }
    return result;
  }
  static double mean(const Json& values, const char* field) {
    double sum = 0; for(const auto& item : values.fields()) sum += number(item.second[field]);
    return values.fields().empty() ? -1e9 : sum / values.fields().size();
  }
  bool improves(const Json& child, const Json& parent) const {
    if(child.fields().empty() || child.fields().size() != parent.fields().size()) return false;
    for(const auto& item : parent.fields()) if(!child.has(item.first)) return false;
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
    const auto experiences = source_experiences(candidate);
    for(const auto& item : experiences.items()) result["experience:" + item.str("memory_id")] = item["content"];
    const auto policy = proto<v1::RuntimePolicy>(candidate["policy"]);
    if(controls.flag("enabled", false) && policy.options().requested_level() >= v1::L2) {
      result["action_rules"] = Json::Array{};
      for(const auto& rule : policy.options().action_rules()) if(rule.has_source_experience()) {
        Json arguments = Json::Array{}; for(const auto& key : rule.required_arguments()) arguments.push(key);
        result["action_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()}, {"operation", rule.operation()},
            {"required_arguments", arguments}, {"reason", rule.reason()}});
      }
      if(policy.options().requested_level() >= v1::L3 && without_generated_controls(policy.options()).loop_rules_size()) {
        result["loop_rules"] = Json::Array{};
        for(const auto& rule : policy.options().loop_rules()) if(rule.has_source_experience())
          result["loop_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()}, {"max_iterations", std::size_t(rule.max_iterations())},
              {"max_no_progress", std::size_t(rule.max_no_progress())}, {"reason", rule.reason()}});
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
    const auto experiences = source_experiences(parent);
    for(const auto& item : experiences.items()) {
      const auto id = item.str("memory_id"), content = values["experience:" + id].text();
      require(!content.empty() && content.size() <= config.num("max_experience_bytes", 2048), "gepa_experience_limit");
      for(const auto& entry : policy.snapshot().experiences()) if(entry.memory_id() == id && entry.content() != content) {
        auto* rewrite = options.add_experience_overrides(); rewrite->set_content(content);
        rewrite->mutable_source_experience()->set_memory_id(id);
        rewrite->mutable_source_experience()->set_content_revision(entry.content_revision());
      }
    }
    std::sort(options.mutable_experience_overrides()->begin(), options.mutable_experience_overrides()->end(),
        [](const auto& a, const auto& b) { return a.source_experience().memory_id() < b.source_experience().memory_id(); });
    if(values.has("action_rules")) {
      Json draft{{"action_rules", values["action_rules"]}, {"loop_rules", values.has("loop_rules") ? values["loop_rules"] : Json(Json::Array{})}};
      options = apply_control_draft(policy.snapshot(), without_generated_controls(options), controls, draft);
    }
    result["policy"] = json(compile_runtime_policy(policy.snapshot(), options));
    require(published_policy_compatible(proto<v1::RuntimePolicy>(result["policy"]),
        proto<v1::RuntimePolicy>(seed["policy"]), controls.dump()), "gepa_candidate_not_authorized");
    agent_version_id(result.dump()); return result;
  }
  Json packet(const Json& parent, const Report& feedback, const Json& selected) const {
    Json result{{"parent_version", agent_version_id(parent.dump())}, {"experiences", source_experiences(parent)},
        {"components", components(parent)}, {"selected_components", selected}, {"feedback", Json::Array{}},
        {"objective", config["objective"]}, {"limits", Json{{"max_experience_bytes", config.num("max_experience_bytes", 2048)}}}};
    for(auto& experience : result["experiences"].items()) experience.fields().erase("content");
    if(controls.flag("enabled", false)) {
      const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
      result["controls"] = control_generation_input(policy.snapshot(), without_generated_controls(policy.options()), controls);
    }
    // Only training content reaches reflection. Validation/holdout scores select
    // candidates but their tasks, rubrics and traces never enter this packet.
    std::vector<const dataset::v1::EvaluationTrial*> trials;
    for(const auto& trial : feedback.trials()) trials.push_back(&trial);
    std::stable_sort(trials.begin(), trials.end(), [](const auto* a, const auto* b) { return (a->verdict() == "failed") > (b->verdict() == "failed"); });
    for(const auto* trial : trials) {
      if(result["feedback"].items().size() >= config.num("max_feedback_trials", 4)) break;
      const dataset::v1::EvaluationTask* task = nullptr;
      for(const auto& value : dataset.tasks()) if(value.task_id() == trial->task_id()) task = &value;
      require(task && task->split() == "train", "gepa_reflection_split_violation");
      Json turns = Json::Array{}, constraints = Json::Array{};
      for(const auto& turn : task->followup_inputs()) turns.push(clip(turn, 4096));
      for(const auto& constraint : task->constraints()) constraints.push(clip(constraint, 4096));
      Json row{{"task_id", task->task_id()}, {"task_input", clip(task->task_input(), 4096)},
          {"followup_inputs", turns}, {"constraints", constraints},
          {"expected_behavior", clip(task->expected_behavior(), 4096)}, {"trial", feedback_trial(*trial)}};
      auto trial_packet = result; trial_packet["feedback"].push(row);
      if(trial_packet.dump().size() > config.num("max_input_bytes", 65536)) {
        row["trial"] = Json{{"verdict", trial->verdict()}, {"checks", excerpt(json(*trial)["checks"])},
            {"output", excerpt(json(trial->evidence())["output"])}, {"metrics", json(trial->metrics())}, {"evidence_truncated", true}};
        trial_packet = result; trial_packet["feedback"].push(row);
      }
      if(trial_packet.dump().size() <= config.num("max_input_bytes", 65536)) result = std::move(trial_packet);
    }
    require(!result["feedback"].items().empty() && result.dump().size() <= config.num("max_input_bytes", 65536), "gepa_feedback_budget_too_small");
    validate_local_data(result); return result;
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
      keys(change, {"memory_id", "content"}); const auto key = "experience:" + change.str("memory_id");
      require(values.has(key), "gepa_experience_not_authorized");
      require(unique.insert(key).second, "gepa_draft_invalid"); values[key] = change["content"];
    }
    if(draft.has("controls")) {
      keys(draft["controls"], {"action_rules", "loop_rules"});
      require(draft["controls"].object() && !draft["controls"].fields().empty(), "gepa_draft_invalid");
      for(const auto& item : draft["controls"].fields()) {
        require(item.second.array() && (values.has(item.first) || item.second.items().empty()), "gepa_component_not_authorized");
        if(values.has(item.first)) values[item.first] = item.second;
      }
    }
    std::set<std::string> allowed; for(const auto& item : selected.items()) allowed.insert(item.text());
    for(const auto& item : values.fields()) if(item.second.dump() != before[item.first].dump())
      require(allowed.count(item.first), "gepa_component_not_authorized");
    return compose(parent, values);
  }
  Json reflect(const Json& input, std::size_t node, const std::string& suffix = {}, bool aggregate = false) {
    require(input.dump().size() <= config.num("max_input_bytes", 65536), "gepa_feedback_budget_too_small");
    check(); require(tree.num("model_calls", 0) < config.num("max_model_calls", 8), "gepa_model_budget_exhausted");
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
      check();
      http.timeout_ms = std::max<std::size_t>(1, std::min<std::size_t>(http.timeout_ms,
          tree.num("started_ms", 0) + config.num("timeout_ms", 1800000) - now()));
      http.cancelled = [&] { return (cancelled && cancelled()) || now() >= tree.num("started_ms", 0) + config.num("timeout_ms", 1800000); };
      http.max_request_bytes = std::max<std::size_t>(http.max_request_bytes, config.num("max_input_bytes", 65536) * 2);
      adapter = legacy::make_openai_compatible_model_adapter(http);
    }
    legacy::ModelRequest request;
    const auto name = std::to_string(node) + suffix;
    request.packet_id = "gepa-" + context.substr(0, 16) + "-" + name;
    request.system_prompt = read(config.str(aggregate ? "aggregation_prompt_file" : "prompt_file"), 65536); request.user_prompt = input.dump();
    validate_local_data(Json(request.system_prompt));
    write(root / "reflections" / (name + ".request.json"), Json{{"system", request.system_prompt}, {"input", input}}.dump());
    const auto response = adapter->evaluate(request); check();
    if(response.error_code == "http_429") {
      // The provider rejected this call; retry the same generation, retaining
      // its reserved budget and already completed execution checkpoints.
      tree["nodes"].items()[node]["status"] = "pending";
      save(); throw Error("gepa_model_rate_limited");
    }
    require(response.transport_ok && response.error_code.empty() && response.body.size() <= 262144, "gepa_reflection_transport_failed");
    auto body = Json::parse(legacy::unwrap_model_json(response.body));
    if(body.has("choices")) {
      require(body["choices"].array() && body["choices"].items().size() == 1, "gepa_reflection_choice_invalid");
      const auto& choice = body["choices"].items().front();
      require(choice.str("finish_reason") == "stop", "gepa_reflection_incomplete");
      body = Json::parse(legacy::unwrap_model_json(choice["message"].str("content")));
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
        const auto candidate = mutate(parent, draft, one);
        write(root / "reflections" / (std::to_string(node) + suffix + ".draft.json"), draft.dump());
        intermediates.push(Json{{"reason", draft["reason"]}, {"component", key}, {"value", components(candidate)[key]}});
        stages.push(Json{{"component", key}, {"phase", "map"}, {"file", std::to_string(node) + suffix}, {"feedback_records", packet["feedback"].items().size()}});
        tree["nodes"].items()[node]["reflection_stages"] = stages; save();
      }
      auto packet = input; packet.fields().erase("feedback");
      packet["phase"] = "combee_reduce"; packet["selected_components"] = one; packet["intermediate_proposals"] = intermediates;
      const auto draft = reflect(packet, node, prefix + "-reduce", true);
      const auto candidate = mutate(parent, draft, one); values[key] = components(candidate)[key];
      write(root / "reflections" / (std::to_string(node) + prefix + "-reduce.draft.json"), draft.dump());
      stages.push(Json{{"component", key}, {"phase", "reduce"}, {"file", std::to_string(node) + prefix + "-reduce"}, {"groups", k}});
      tree["nodes"].items()[node]["reflection_stages"] = stages; save();
      if(key.compare(0, 11, "experience:") == 0)
        drafts["experiences"].push(Json{{"memory_id", key.substr(11)}, {"content", values[key]}});
      else drafts["controls"][key] = values[key];
    }
    return drafts;
  }
  std::size_t winner() const {
    std::size_t best = 0;
    const auto& nodes = tree["nodes"].items();
    for(std::size_t i = 1; i < nodes.size(); ++i) if(nodes[i].str("status") == "accepted"
        && improves(nodes[i]["validation"], nodes[best]["validation"])) best = i;
    return best;
  }
  void proposal(std::size_t index, const Json& candidate) {
    auto& node = tree["nodes"].items()[index]; const auto version = agent_version_id(candidate.dump());
    node["version"] = version; node["status"] = "proposed";
    for(std::size_t j = 0; j < index; ++j) if(tree["nodes"].items()[j].str("version") == version) {
      node["duplicate_of"] = j; node["status"] = "duplicate"; break;
    }
    write(root / "versions" / (std::to_string(index) + ".json"), candidate.dump()); save();
  }
  void error(std::size_t index, const std::exception& exception) {
    const std::string code = exception.what();
    if(code == "gepa_cancelled" || code == "gepa_time_budget_exhausted"
        || code == "gepa_model_rate_limited") { save(); throw Error(code.c_str()); }
    auto& node = tree["nodes"].items()[index]; node["error_stage"] = node["status"]; node["status"] = "error";
    // Persist fixed codes only; model/transport diagnostics can contain secrets.
    const std::set<std::string> codes{"gepa_trial_budget_exhausted", "gepa_parent_evaluation_inconclusive",
        "gepa_feedback_budget_too_small", "gepa_reflection_transport_failed", "gepa_reflection_incomplete",
        "gepa_experience_not_authorized", "gepa_experience_limit", "gepa_draft_invalid", "gepa_candidate_not_authorized",
        "gepa_model_not_configured", "gepa_model_key_missing", "credential_value_rejected", "sensitive_field_rejected",
        "invalid_json", "control_operation_not_authorized", "control_argument_not_authorized", "control_loop_relaxes_limit",
        "control_draft_limits_invalid", "control_duplicate_loop_scope", "gepa_component_not_authorized", "gepa_components_changed",
        "gepa_model_budget_exhausted"};
    node["error"] = codes.count(code) ? code : "gepa_proposal_or_evaluation_failed";
    if(code == "gepa_trial_budget_exhausted") tree["stop_reason"] = code;
    if(code == "gepa_model_budget_exhausted") tree["reflection_exhausted"] = true;
    if(code == "gepa_model_not_configured" || code == "gepa_model_key_missing") {
      tree["status"] = "error"; tree["active"] = nullptr; save(); throw Error(code.c_str());
    }
    save();
  }
  void validate(std::size_t index) {
    try {
      const auto report = evaluate(artifact(index), val, "val"); const auto values = scores(report);
      auto& node = tree["nodes"].items()[index]; node["validation"] = values;
      node["status"] = values.fields().empty() ? "inconclusive" : "accepted"; save();
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
      if(tree.str("stop_reason") == "gepa_trial_budget_exhausted") { node["status"] = "skipped_budget"; continue; }
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
      const auto values = scores(evaluate(artifact(index), tasks, "val"));
      Json first = Json::Object{}, second = Json::Object{};
      for(const auto& id : tasks) {
        first[id] = tree["nodes"].items()[ids[0]]["validation"][id];
        second[id] = tree["nodes"].items()[ids[1]]["validation"][id];
      }
      node["screening"] = Json{{"parents", Json::Array{first, second}}, {"candidate", values}, {"split", "val"}};
      node["status"] = values.fields().empty() ? "inconclusive"
          : gepa::no_worse(values, first) && gepa::no_worse(values, second) ? "screened" : "rejected_merge";
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
    require(config.num("max_trials", 400) >= reserve + val.size() * evaluation.num("repetitions", 1), "gepa_holdout_budget_required");
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
      tree = Json{{"schema_version", 2}, {"algorithm", "gepa-reflective-dag-v2"}, {"context", context},
          {"seed_version", agent_version_id(seed.dump())}, {"baseline_version", agent_version_id(baseline.dump())},
          {"dataset_version", dataset.dataset_version()}, {"started_ms", now()}, {"rounds", 0}, {"model_calls", 0},
          {"trials_used", 0}, {"active", nullptr}, {"evaluations", Json::Object{}}, {"nodes", Json::Array{
            Json{{"id", 0}, {"parent", nullptr}, {"parents", Json::Array{}}, {"kind", "seed"}, {"depth", 0},
                {"version", agent_version_id(seed.dump())}, {"status", "seed"}, {"selections", 0}}}}};
      write(root / "versions/0.json", seed.dump()); save();
    }
    if(!tree.has("merge_attempts")) tree["merge_attempts"] = Json::Array{};
    if(!tree.has("batch")) tree["batch"] = nullptr;
    tree["schema_version"] = 2; tree["algorithm"] = "gepa-reflective-dag-v2";
    if(tree["nodes"].items()[0].str("status") == "seed") {
      const auto report = evaluate(seed, val, "val"); const auto values = scores(report);
      require(!values.fields().empty(), "gepa_seed_evaluation_inconclusive");
      tree["nodes"].items()[0]["validation"] = values; tree["nodes"].items()[0]["status"] = "accepted"; save();
    }
    if(!tree.has("frontier") || tree["frontier"].null()) {
      tree["frontier"] = Json::Array{}; for(auto id : frontier()) tree["frontier"].push(id);
      tree["frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config);
    }
    tree["status"] = "searching"; save();
    while(!tree.flag("holdout_started", false) && (!tree["active"].null() || !tree["batch"].null()
        || tree.num("rounds", 0) < config.num("max_rounds", 8))) {
      check();
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
            auto& previous = tree["nodes"].items()[p];
            Json plan{{"parent", p}, {"tasks", Json::Array{}},
                {"components", gepa::select_components(components(artifact(p)), config.str("component_selector", "round_robin"), previous.num("component_cursor", 0))},
                {"component_cursor", previous.num("component_cursor", 0) + 1}};
            for(const auto& id : minibatch(sequence + groups)) plan["tasks"].push(id);
            previous["component_cursor"] = plan["component_cursor"]; ++groups;
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
          if(node.str("status") == "pending") {
            bool perfect = config.flag("skip_perfect_score", false);
            for(const auto& item : parent_scores.fields()) perfect &= number(item.second["quality"]) >= 1
                && number(item.second["fitness"]) >= number(config["perfect_score"], 1);
            if(perfect) { node["status"] = "skipped_perfect"; save(); }
            else {
              const auto selected = node.has("components") ? node["components"] : gepa::select_components(components(parent_artifact), "all", 0);
              auto input = packet(parent_artifact, parent_report, selected);
              const auto draft = propose(parent_artifact, input, index);
              const auto candidate = mutate(parent_artifact, draft, selected);
              node["reason"] = draft.str("reason");
              write(root / "reflections" / (std::to_string(index) + ".draft.json"), draft.dump());
              proposal(index, candidate);
            }
          }
          if(node.str("status") == "proposed") {
            const auto child = artifact(index); const auto screening = evaluate(child, batch, "train"); const auto values = scores(screening);
            node["screening"] = Json{{"parent", parent_scores}, {"candidate", values}};
            const bool pass = config.str("acceptance", "strict_improvement") == "improvement_or_equal"
                ? gepa::no_worse(values, parent_scores) : improves(values, parent_scores);
            node["status"] = values.fields().empty() ? "inconclusive" : pass ? "screened" : "rejected_minibatch"; save();
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
      if(tree.str("stop_reason") == "gepa_trial_budget_exhausted" && tree["batch"].null()) break;
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
    return {selected, report, summary};
  }
};
}  // namespace

Json load_gepa_config(const fs::path& path) {
  auto config = Json::parse(read(path));
  keys(config, {"schema_version", "runtime_config", "prompt_file", "max_rounds", "max_model_calls", "max_trials", "timeout_ms",
      "minibatch_size", "max_feedback_trials", "max_input_bytes", "max_experience_bytes", "patience", "seed", "min_improvement", "objective",
      "use_merge", "max_merge_invocations", "merge_minibatch_size", "candidate_selection_strategy", "frontier_type", "prune_dominated",
      "epsilon", "top_k", "component_selector", "minibatch_strategy", "skip_perfect_score", "perfect_score", "acceptance",
      "proposals_per_round", "proposal_selection", "proposal_top_k", "cache_evaluations",
      "sampling_strategy", "parents_per_round", "target_fitness",
      "reflection_strategy", "duplication_factor", "aggregation_prompt_file", "unknown_token_policy"});
  require(config.num("schema_version", 0) == 1, "gepa_config_invalid");
  bound(config, "max_rounds", 8, 1, 256); bound(config, "max_model_calls", 8, 1, 256);
  bound(config, "max_trials", 400, 4, 10000); bound(config, "timeout_ms", 1800000, 1000, 86400000);
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
  choice("acceptance", "strict_improvement", {"strict_improvement", "improvement_or_equal"});
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
  config["config_file"] = fs::absolute(path).lexically_normal().string();
  validate_local_data(config); return config;
}

std::string gepa_config_fingerprint(const Json& config) {
  if(config.null()) return {};
  auto runtime = legacy::load_runtime_configuration(config.str("runtime_config"));
  std::string bytes = "gepa-reflective-dag-v2:" + config.dump() + read(config.str("config_file")) + read(config.str("prompt_file"));
  if(config.has("aggregation_prompt_file")) bytes += read(config.str("aggregation_prompt_file"));
  for(const auto& path : runtime.source_files) bytes += read(path);
  if(runtime.model.endpoint.empty()) if(const auto* base = std::getenv("OPENAI_BASE_URL")) bytes += base;
  if(runtime.model.model_id.empty()) if(const auto* model = std::getenv("OPENAI_MODEL")) bytes += model;
  return hash(bytes);
}

GepaResult run_gepa_search(const Json& seed, const Json& baseline, const fs::path& dataset,
    const fs::path& evaluation_path, const Json& evaluation, const Json& config, const Json& controls,
    const fs::path& output, const std::string& context, const std::function<bool()>& cancelled,
    std::shared_ptr<legacy::ModelAdapter> reflection_model) {
  Search search;
  search.seed = seed; search.baseline = baseline; search.dataset_path = dataset; search.evaluation_path = evaluation_path;
  search.evaluation = evaluation; search.config = config; search.controls = controls; search.root = output;
  search.context = context; search.cancelled = cancelled; search.model = std::move(reflection_model);
  return search.run();
}
}  // namespace trajectory::evolution::integration
