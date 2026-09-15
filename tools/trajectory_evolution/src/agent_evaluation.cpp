#include "trajectory_evolution/agent_evaluation.h"
#include "trajectory_evolution/task_evaluation.h"
#include "integration_json.h"
#include "local_support.h"
#include "control_generation.h"
#include "gepa_search.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <openssl/sha.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace trajectory::evolution {
namespace {
using namespace integration;
namespace fs = std::filesystem;
std::string digest(const std::string& bytes) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), out), "evaluation_hash_failed");
  std::string result;
  for(auto c : out) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
  return result;
}
std::size_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string read(const fs::path& path, std::size_t limit = 4194304) {
  fs::path prefix;
  for(const auto& part : fs::absolute(path)) { prefix /= part; require(!fs::is_symlink(prefix), "evaluation_symlink_rejected"); }
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(stream && stream.tellg() >= 0 && static_cast<std::size_t>(stream.tellg()) <= limit, "evaluation_file_unavailable");
  std::string bytes(static_cast<std::size_t>(stream.tellg()), '\0'); stream.seekg(0);
  require(bytes.empty() || bool(stream.read(bytes.data(), bytes.size())), "evaluation_read_failed");
  return bytes;
}
void directory(const fs::path& path) {
  fs::path prefix;
  for(const auto& part : fs::absolute(path)) { prefix /= part; require(!fs::is_symlink(prefix), "evaluation_symlink_rejected"); }
  fs::create_directories(path); require(::chmod(path.c_str(), 0700) == 0, "evaluation_permission_failed");
}
void write(const fs::path& path, const std::string& bytes) {
  directory(path.parent_path()); require(!fs::is_symlink(path), "evaluation_symlink_rejected");
  std::string pending = path.string() + ".pending.XXXXXX";
  int fd = ::mkstemp(pending.data()); require(fd >= 0, "evaluation_write_failed");
  try {
    std::size_t offset = 0;
    while(offset < bytes.size()) { const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset); require(n > 0, "evaluation_write_failed"); offset += n; }
    require(::fsync(fd) == 0, "evaluation_write_failed"); ::close(fd); fd = -1;
    require(::rename(pending.c_str(), path.c_str()) == 0, "evaluation_publish_failed");
    int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if(parent >= 0) { ::fsync(parent); ::close(parent); }
  } catch(...) { if(fd >= 0) ::close(fd); ::unlink(pending.c_str()); throw; }
}
double numeric(const Json& value, double fallback = 0) {
  return std::holds_alternative<double>(value.value) ? std::get<double>(value.value) : fallback;
}
Json canonical(Json artifact) {
  if(artifact["policy"].object()) {
    auto& policy = artifact["policy"];
    policy.fields().erase("policy_id");
    auto& snapshot = policy["snapshot"];
    for(const auto* key : {"snapshot_id", "memory_revision", "shared_revision", "excluded_counts"}) snapshot.fields().erase(key);
    snapshot["identity"].fields().erase("run_id"); snapshot["identity"].fields().erase("conversation_id");
    policy["options"].fields().erase("allowed_conversations");
  }
  return artifact;
}
std::string lane(const std::string& user, const std::string& level) { return digest(user + '\n' + level); }
Json baseline(const Json& artifact) {
  return Json{{"schema_version", 1}, {"agent", artifact["agent"]}, {"user_id", artifact["user_id"]}, {"level", "L0"}, {"policy", nullptr}};
}
Json memories(const Json& artifact) {
  Json result = Json::Object{};
  const auto& policy = artifact["policy"];
  if(!policy.object() || !policy["options"]["approved_experiences"].array()) return result;
  for(const auto& approval : policy["options"]["approved_experiences"].items()) {
    const auto& experiences = policy["snapshot"]["experiences"];
    if(!experiences.array()) continue;
    for(const auto& item : experiences.items()) if(item.str("memory_id") == approval.str("memory_id")
        && item.str("content_revision") == approval.str("content_revision")) result[item.str("memory_id")] = item;
  }
  return result;
}
}  // namespace

std::string agent_version_id(const std::string& bytes) {
  require(bytes.size() <= 2097152, "agent_version_too_large");
  auto artifact = Json::parse(bytes);
  keys(artifact, {"schema_version", "agent", "user_id", "level", "policy"});
  require(artifact.num("schema_version", 0) == 1 && identifier(artifact.str("user_id"))
      && artifact["agent"].object() && !artifact["agent"].str("revision").empty(), "agent_version_invalid");
  const auto level = artifact.str("level");
  require(level.size() == 2 && level[0] == 'L' && level[1] >= '0' && level[1] <= '3', "agent_version_level_invalid");
  validate_local_data(artifact);
  if(level == "L0") require(artifact["policy"].null(), "baseline_policy_must_be_empty");
  else {
    PolicyRuntime policy(proto<v1::RuntimePolicy>(artifact["policy"]));
    require(policy.policy().snapshot().identity().user_id() == artifact.str("user_id")
        && int(policy.policy().effective_level()) == int(v1::L0) + level[1] - '0', "agent_version_policy_mismatch");
  }
  return "agent-version-v1:" + digest(canonical(std::move(artifact)).dump());
}

v1::RuntimePolicy bind_runtime_policy(const v1::RuntimePolicy& policy, const v1::Identity& identity) {
  PolicyRuntime checked(policy);
  auto options = policy.options();
  options.clear_allowed_conversations(); options.add_allowed_conversations(identity.conversation_id());
  return compile_runtime_policy(bind_shadow_policy(policy.snapshot(), identity), options);
}
bool published_policy_compatible(const v1::RuntimePolicy& published, const v1::RuntimePolicy& current,
    const std::string& control_learning_json) {
  if(published.effective_level() != current.effective_level()) return false;
  auto old = bind_runtime_policy(published, current.snapshot().identity());
  const auto controls = control_learning_json.empty() ? Json() : Json::parse(control_learning_json);
  if(!generated_controls_authorized(old, current, controls)) return false;
  auto a = old.options(), b = current.options();
  a = without_generated_controls(a); b = without_generated_controls(b);
  a.clear_approved_experiences(); b.clear_approved_experiences();
  a.clear_experience_overrides(); b.clear_experience_overrides();
  if(json(a).dump() != json(b).dump() || json(old.snapshot().options()).dump() != json(current.snapshot().options()).dump()) return false;
  for(const auto& approved : old.options().approved_experiences()) {
    bool allowed = false, present = false;
    for(const auto& other : current.options().approved_experiences())
      allowed |= approved.memory_id() == other.memory_id() && approved.content_revision() == other.content_revision();
    for(const auto& item : current.snapshot().experiences())
      present |= approved.memory_id() == item.memory_id() && approved.content_revision() == item.content_revision();
    if(!allowed || !present) return false;
  }
  // A changed relation can alter joint eligibility even when both texts survive.
  if(old.snapshot().cross_relations_size() != current.snapshot().cross_relations_size()) return false;
  for(int i = 0; i < old.snapshot().cross_relations_size(); ++i)
    if(json(old.snapshot().cross_relations(i)).dump() != json(current.snapshot().cross_relations(i)).dump()) return false;
  return true;
}

struct AgentEvaluationService::Impl {
  struct Lock {
    int fd = -1;
    ~Lock() { if(fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
  } file_lock;
  fs::path root, config_path, dataset_path, evaluation_path;
  Json config, evaluation, state, gepa, controls, published = Json::Object{};
  std::string context;
  std::function<void()> changed;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stopping{false}, cancel_running{false};
  std::thread worker;
  std::size_t max_jobs = 256, interval = 60000, hourly = 12;

  std::string fingerprint() const {
    std::string bytes = read(config_path) + read(evaluation_path);
    const auto data = read_evaluation_dataset(dataset_path);
    for(const auto& issue : data.issues()) require(issue.severity() != "error", "evaluation_dataset_invalid");
    bytes += data.dataset_version();
    if(!gepa.null()) bytes += gepa_config_fingerprint(gepa) + controls.dump();
    for(const auto& variant : evaluation["variants"].items())
      if(variant.has("instrumentation_config")) bytes += read(variant.str("instrumentation_config"));
    if(evaluation["judge"].flag("enabled", false)) {
      bytes += read(evaluation["judge"].str("prompt"));
      if(evaluation["judge"].has("loop_review_prompt")) bytes += read(evaluation["judge"].str("loop_review_prompt"));
      bytes += read(evaluation["judge"].str("runtime_config"));
    }
    // Missing activity state now uses the current request, changing execution.
    return digest("boundary-current-request-v1\n" + bytes);
  }
  explicit Impl(const fs::path& file, const fs::path& data, std::function<void()> callback, const std::string& control_json)
      : root(fs::absolute(data)), config_path(fs::absolute(file)), changed(std::move(callback)) {
    controls = control_json.empty() ? Json() : Json::parse(control_json);
    validate_control_learning(controls);
    config = Json::parse(read(config_path));
    keys(config, {"schema_version", "dataset", "evaluation_config", "agent", "min_interval_ms", "max_jobs", "max_jobs_per_hour", "gate", "gepa_config", "training"});
    require(config.num("schema_version", 0) == 1 && config["agent"].object() && !config["agent"].str("revision").empty(), "agent_evaluation_config_invalid");
    validate_local_data(config);
    auto resolve = [&](const std::string& name) { fs::path p(config.str(name)); require(!p.empty(), "evaluation_path_required"); return p.is_absolute() ? p : config_path.parent_path() / p; };
    dataset_path = resolve("dataset"); evaluation_path = resolve("evaluation_config");
    if(config.has("gepa_config")) gepa = load_gepa_config(resolve("gepa_config"));
    if(!config.has("training")) config["training"] = Json::Object{};
    const auto& training = config["training"];
    keys(training, {"enabled", "min_experiences", "batch_size", "max_batches"});
    if(training.has("enabled")) training.flag("enabled", true);
    for(const auto* key : {"min_experiences", "batch_size", "max_batches"})
      require(training.num(key, 1) > 0 && training.num(key, 1) <= 512, "training_limit_invalid");
    evaluation = Json::parse(read(evaluation_path));
    evaluation["require_trace"] = true;
    require(evaluation["variants"].items().size() == 1, "evaluation_host_must_be_single");
    require(evaluation.num("parallelism", 1) <= 4, "background_evaluation_parallelism_limit");
    if(!evaluation.has("resources")) evaluation["resources"] = Json{{"nice", 10}, {"address_space_bytes", std::size_t(1073741824)}, {"cpu_seconds", 300}};
    if(evaluation["variants"].items()[0].has("instrumentation_config")) {
      const fs::path resource(evaluation["variants"].items()[0].str("instrumentation_config"));
      if(resource.is_relative()) evaluation["variants"].items()[0]["instrumentation_config"] = (evaluation_path.parent_path() / resource).string();
    }
    // Host commands and environment names remain deployment-controlled.
    for(auto& argument : evaluation["variants"].items()[0]["command"].items()) {
      auto value = argument.text(); const std::string marker = "{config_dir}";
      for(std::size_t i; (i = value.find(marker)) != std::string::npos;) value.replace(i, marker.size(), evaluation_path.parent_path().string());
      argument = value;
    }
    if(evaluation["judge"].flag("enabled", false)) for(const auto* key : {"prompt", "runtime_config", "loop_review_prompt"}) {
      if(std::string(key) == "loop_review_prompt" && !evaluation["judge"].has(key)) continue;
      fs::path p(evaluation["judge"].str(key)); if(p.is_relative()) p = evaluation_path.parent_path() / p;
      evaluation["judge"][key] = p.string();
    }
    max_jobs = config.num("max_jobs", 256); interval = config.num("min_interval_ms", 60000); hourly = config.num("max_jobs_per_hour", 12);
    require(max_jobs >= 4 && max_jobs <= 1024 && interval <= 86400000 && hourly > 0 && hourly <= 1000, "evaluation_queue_limits_invalid");
    const auto& gate = config["gate"];
    keys(gate, {"min_pass_rate", "min_coverage", "max_pass_rate_drop", "min_trials", "max_latency_ratio", "max_token_ratio", "min_invalid_loop_reduction", "memory_min_pairs"});
    for(const auto* key : {"min_pass_rate", "min_coverage", "max_pass_rate_drop", "min_invalid_loop_reduction"})
      if(gate.has(key)) require(numeric(gate[key], -1) >= 0 && numeric(gate[key], -1) <= 1, "evaluation_gate_invalid");
    require(gate.num("min_trials", 2) > 0 && gate.num("min_trials", 2) <= 10000, "evaluation_gate_invalid");
    require(gate.num("memory_min_pairs", 2) > 0 && gate.num("memory_min_pairs", 2) <= 10000, "evaluation_gate_invalid");
    for(const auto* key : {"max_latency_ratio", "max_token_ratio"})
      if(gate.has(key)) require(numeric(gate[key], -1) > 0, "evaluation_gate_invalid");
    if(!gepa.null()) {
      const auto data = read_evaluation_dataset(dataset_path);
      std::size_t count = 0;
      for(const auto& task : data.tasks()) count += task.split() == "holdout";
      require(count * evaluation.num("repetitions", 1) >= gate.num("min_trials", 2), "gepa_holdout_trials_below_gate");
    }
    context = fingerprint(); directory(root);
    file_lock.fd = ::open((root / "evaluation.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    require(file_lock.fd >= 0 && ::flock(file_lock.fd, LOCK_EX | LOCK_NB) == 0, "evaluation_directory_in_use");
    state = Json{{"schema_version", 1}, {"jobs", Json::Object{}}, {"latest", Json::Object{}}, {"publications", Json::Object{}}, {"feedback", Json::Object{}}, {"starts", Json::Array{}}, {"next_due_ms", 0}};
    if(fs::exists(root / "state.json")) state = Json::parse(read(root / "state.json", 16777216));
    require(state.num("schema_version", 0) == 1 && state["jobs"].fields().size() <= max_jobs, "evaluation_state_invalid");
    for(auto& pair : state["jobs"].fields()) {
      require(pair.first.size() == 64 && pair.first.find_first_not_of("0123456789abcdef") == std::string::npos, "evaluation_job_invalid");
      if(pair.second.str("status") == "running") {
        const bool resume = pair.second.flag("gepa", false) && pair.second.str("context") == context
            && !pair.second.flag("cancel_requested", false);
        pair.second["status"] = resume ? "pending" : "interrupted";
        if(!resume) pair.second["error"] = "evaluation_interrupted_retry_required";
      }
      if(pair.second.str("status") == "pending" && pair.second.str("context") != context) pair.second["status"] = "superseded";
    }
    for(const auto& p : state["publications"].fields()) {
      if(p.second.str("context") != context) continue;
      const auto artifact = load_artifact(p.second.str("job_id"));
      const auto& job = state["jobs"][p.second.str("job_id")];
      require(agent_version_id(artifact.dump()) == p.second.str("version") && job.str("status") == "passed"
          && job["gate"].flag("passed", false) && job.str("user") == artifact.str("user_id")
          && p.first == lane(job.str("user"), artifact.str("level")), "evaluation_publication_changed");
      published[p.first] = artifact;
    }
    state.fields().erase("worker_error");
    save();
  }
  Json load_artifact(const std::string& id) const {
    require(state["jobs"].has(id), "evaluation_job_not_found", 404);
    return Json::parse(read(root / id / "candidate.json"));
  }
  void save() {
    const auto bytes = state.dump(); require(bytes.size() <= 16777216, "evaluation_state_limit"); write(root / "state.json", bytes);
  }
  void make_room() {
    // Bound the live index without discarding reproducible reports or dedup IDs.
    while(state["jobs"].fields().size() >= max_jobs) {
      std::string oldest; std::size_t time = SIZE_MAX;
      for(const auto& p : state["jobs"].fields()) {
        if(p.second.str("status") == "pending" || p.second.str("status") == "running") continue;
        bool pinned = state["latest"].str(lane(p.second.str("user"), p.second.str("level"))) == p.first;
        if(state["training"].object()) for(const auto& training : state["training"].fields())
          pinned |= training.second.str("job_id") == p.first;
        for(const auto& publication : state["publications"].fields()) pinned |= publication.second.str("job_id") == p.first;
        if(!pinned && p.second.num("queued_ms", 0) < time) { oldest = p.first; time = p.second.num("queued_ms", 0); }
      }
      require(!oldest.empty(), "evaluation_queue_capacity", 507);
      write(root / oldest / "job.json", state["jobs"][oldest].dump());
      state["jobs"].fields().erase(oldest);
    }
  }
  Json gate_result(const dataset::v1::TaskEvaluationReport& report) const {
    const auto summary = json(report.summary()); const auto& groups = summary["by_variant"];
    const auto& candidate = groups["candidate"];
    const auto& base = groups.has("baseline") ? groups["baseline"] : candidate;
    const auto& gate = config["gate"]; Json reasons = Json::Array{};
    auto reject = [&](bool condition, const char* code) { if(condition) reasons.push(code); };
    reject(!report.complete(), "evaluation_incomplete");
    reject(candidate.num("trials", 0) < gate.num("min_trials", 2), "insufficient_trials");
    const double coverage = numeric(gate["min_coverage"], 1.0);
    reject(numeric(candidate["coverage"], -1) < coverage || numeric(base["coverage"], -1) < coverage, "insufficient_coverage");
    reject(numeric(candidate["pass_rate"], -1) < numeric(gate["min_pass_rate"], 0.8), "quality_below_threshold");
    reject(numeric(candidate["pass_rate"], -1) + numeric(gate["max_pass_rate_drop"], 0) < numeric(base["pass_rate"], 0), "quality_regression");
    auto ratio = [&](const char* option, const Json& a, const Json& b) {
      if(groups.has("baseline") && gate.has(option)) reject(a.null() || b.null() || numeric(a) > numeric(b) * numeric(gate[option]), option);
    };
    ratio("max_latency_ratio", candidate["mean_agent_duration_ms"], base["mean_agent_duration_ms"]);
    ratio("max_token_ratio", candidate["host_reported_metrics"]["tokens"]["mean"], base["host_reported_metrics"]["tokens"]["mean"]);
    if(groups.has("baseline") && gate.has("max_token_ratio")) reject(candidate["host_reported_metrics"]["tokens"].num("observations", 0) != candidate.num("trials", 0)
        || base["host_reported_metrics"]["tokens"].num("observations", 0) != base.num("trials", 0), "token_metric_unobserved");
    if(groups.has("baseline") && gate.has("min_invalid_loop_reduction")) {
      const auto& a = candidate.has("invalid_loop_assessment") ? candidate["invalid_loop_assessment"] : candidate["invalid_loop_lower_bound"];
      const auto& b = base.has("invalid_loop_assessment") ? base["invalid_loop_assessment"] : base["invalid_loop_lower_bound"];
      reject(a.str("method", "rule_lower_bound") != b.str("method", "rule_lower_bound"), "loop_metric_mismatch");
      reject(a.num("observations", 0) != candidate.num("trials", 0) || b.num("observations", 0) != base.num("trials", 0)
          || a["mean"].null() || b["mean"].null(), "loop_metric_unobserved");
      reject(numeric(a["mean"]) > numeric(b["mean"]) * (1 - numeric(gate["min_invalid_loop_reduction"])), "loop_target_not_met");
      reject(numeric(gate["min_invalid_loop_reduction"]) > 0 && numeric(b["mean"]) <= 0, "loop_baseline_has_no_observed_waste");
    }
    const bool scored = numeric(candidate["coverage"], -1) >= coverage && numeric(base["coverage"], -1) >= coverage
        && candidate.num("scored", 0) >= gate.num("min_trials", 2) && base.num("scored", 0) >= gate.num("min_trials", 2);
    reject(!scored, "insufficient_scored_trials");
    const bool paired = !groups.has("baseline") || json(report.comparisons())["candidate"].num("paired_trials", 0) >= gate.num("min_trials", 2);
    reject(!paired, "insufficient_paired_trials");
    return Json{{"passed", reasons.items().empty()}, {"scored", scored && paired}, {"reasons", reasons}, {"baseline", base}, {"candidate", candidate},
        {"comparison", groups.has("baseline") ? "paired" : "baseline_only"}};
  }
  void record_feedback(const std::string& id, const Json& job, const Json& artifact, const Json& base,
                       const Json& verdict, const dataset::v1::TaskEvaluationReport& report) {
    // Bundle evidence is recorded separately from causal, per-memory validation.
    auto& feedback = state["feedback"][job.str("user")];
    if(!feedback.object()) feedback = Json::Object{};
    const auto referenced = memories(artifact);
    const auto previous = memories(base);
    auto without_experiences = [](Json value) {
      value = canonical(std::move(value));
      if(value["policy"].object()) {
        value["policy"]["options"]["approved_experiences"] = Json::Array{};
        value["policy"]["snapshot"]["experiences"] = Json::Array{};
      }
      return value.dump();
    };
    std::set<std::string> changed_ids;
    for(const auto& item : referenced.fields()) if(item.second.dump() != previous[item.first].dump()) changed_ids.insert(item.first);
    for(const auto& item : previous.fields()) if(item.second.dump() != referenced[item.first].dump()) changed_ids.insert(item.first);
    const bool single_change = changed_ids.size() == 1 && without_experiences(artifact) == without_experiences(base);
    for(const auto& item : referenced.fields()) {
      const auto key = digest(item.first + '\n' + item.second.str("content_revision"));
      auto& value = state["feedback"][job.str("user")][key];
      if(!value.object() || value.str("context") != context) value = Json{{"context", context}, {"memory_id", item.first}, {"content_revision", item.second["content_revision"]},
          {"application_shared", item.second.flag("application_shared", false)}, {"reports", Json::Object{}}};
      std::size_t positive = 0, negative = 0;
      if(single_change && changed_ids.count(item.first)) {
        for(const auto& trial : report.trials()) {
          if(trial.variant_id() != "candidate" || (trial.verdict() != "passed" && trial.verdict() != "failed")) continue;
          bool applied = false;
          const auto evidence = json(trial.evidence());
          const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
          for(const auto& trace : traces.items()) if(trace["events"].array()) for(const auto& event : trace["events"].items())
            if(event.str("kind") == "advice.applied" && event["application"].str("status") == "applied"
                && event["attributes"]["receipt"]["experiences"].array())
              for(const auto& ref : event["attributes"]["receipt"]["experiences"].items())
                applied |= ref.str("memory_id") == item.first && ref.str("content_revision") == item.second.str("content_revision");
          if(!applied) continue;
          for(const auto& parent : report.trials()) if(parent.variant_id() == "baseline" && parent.task_id() == trial.task_id()
              && parent.repetition() == trial.repetition() && (parent.verdict() == "passed" || parent.verdict() == "failed")) {
            positive += trial.verdict() == "passed" && parent.verdict() == "failed";
            negative += trial.verdict() == "failed" && parent.verdict() == "passed";
          }
        }
      }
      value["reports"][id] = Json{{"version", job["version"]}, {"passed", verdict["passed"]},
          {"attribution", positive || negative ? "single_memory_paired" : "version_bundle"},
          {"positive_trials", positive}, {"negative_trials", negative}, {"finished_ms", now_ms()}};
      while(value["reports"].fields().size() > 32) {
        auto& rows = value["reports"].fields();
        rows.erase(std::min_element(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
          return a.second.num("finished_ms", 0) < b.second.num("finished_ms", 0); }));
      }
      value["finished_ms"] = now_ms();
      bool passed = false, failed = false; positive = negative = 0;
      for(const auto& r : value["reports"].fields()) {
        passed |= r.second.flag("passed", false); failed |= !r.second.flag("passed", false);
        positive += r.second.num("positive_trials", 0); negative += r.second.num("negative_trials", 0);
      }
      value["positive_trial_count"] = positive; value["negative_trial_count"] = negative;
      const auto minimum = config["gate"].num("memory_min_pairs", 2);
      value["status"] = positive && negative ? "conflicting_trial_results" : negative >= minimum ? "regressed"
          : positive >= minimum ? "validated" : passed && failed ? "mixed_version_results" : passed ? "supported_in_version" : "review_needed";
    }
    while(feedback.fields().size() > 1024) {
      auto& rows = feedback.fields();
      rows.erase(std::min_element(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second.num("finished_ms", 0) < b.second.num("finished_ms", 0); }));
    }
  }
  void loop() {
    while(!stopping) {
      std::string id; Json job, artifact, base;
      {
        std::unique_lock<std::mutex> guard(mutex);
        for(;;) {
          if(stopping) return;
          std::size_t oldest = SIZE_MAX;
          for(const auto& p : state["jobs"].fields()) if(p.second.str("status") == "pending"
              && p.second.num("queued_ms", 0) < oldest) { id = p.first; oldest = p.second.num("queued_ms", 0); }
          if(id.empty()) { wake.wait(guard); continue; }
          const auto now = now_ms(); auto& starts = state["starts"].items();
          starts.erase(std::remove_if(starts.begin(), starts.end(), [&](const Json& t) { return t.integer() + 3600000 <= now; }), starts.end());
          auto due = state.num("next_due_ms", 0);
          if(starts.size() >= hourly) due = std::max(due, starts.front().integer() + 3600000);
          if(due > now) { id.clear(); wake.wait_for(guard, std::chrono::milliseconds(due - now)); continue; }
          auto& selected = state["jobs"][id]; selected["status"] = "running";
          selected["attempt"] = selected.num("attempt", 0) + 1; selected["started_ms"] = now;
          state["next_due_ms"] = now + interval; starts.push_back(now); save();
          job = selected; artifact = load_artifact(id); base = Json::parse(read(root / id / "baseline.json"));
          cancel_running = false; break;
        }
      }
      Json verdict; std::string error; bool completed = false; dataset::v1::TaskEvaluationReport report;
      try {
        require(fingerprint() == context, "evaluation_configuration_changed");
        TaskEvaluationOptions options;
        options.cancelled = [&] { return stopping || cancel_running; };
        for(auto attempt = job.num("attempt", 0); attempt > 1; --attempt) {
          const auto previous = root / id / ("attempt-" + std::to_string(attempt - 1));
          if(fs::exists(previous / "manifest.json")) { options.resume_report = previous / "report.json"; break; }
        }
        if(job.flag("training", false)) {
          auto settings = evaluation; auto host = evaluation["variants"].items().front();
          host["id"] = "training"; host["version"] = job["version"]; host["artifact"] = artifact;
          settings["variants"] = Json::Array{host}; settings["splits"] = Json::Array{"train"};
          settings["task_ids"] = job["task_ids"];
          options.config_json = settings.dump();
          report = run_task_evaluation(dataset_path, evaluation_path,
              root / id / ("attempt-" + std::to_string(job.num("attempt", 0))), options);
          require(report.complete(), "training_report_incomplete");
        } else if(job.flag("gepa", false)) {
          const auto seed = Json::parse(read(root / id / "seed.json"));
          const auto result = run_gepa_search(seed, base, dataset_path, evaluation_path, evaluation,
              gepa, controls, root / id / "search", context, [&] { return stopping || cancel_running; });
          artifact = result.artifact; report = result.holdout; job["search"] = result.summary;
        } else {
          auto settings = evaluation; auto host = evaluation["variants"].items().front();
          auto a = host, b = host;
          a["id"] = "baseline"; a["version"] = agent_version_id(base.dump()); a["artifact"] = base;
          b["id"] = "candidate"; b["version"] = job["version"]; b["artifact"] = artifact;
          settings["variants"] = artifact.str("level") == "L0" ? Json(Json::Array{b}) : Json(Json::Array{a, b});
          options.config_json = settings.dump();
          report = run_task_evaluation(dataset_path, evaluation_path,
              root / id / ("attempt-" + std::to_string(job.num("attempt", 0))), options);
        }
        require(!stopping && !cancel_running, "evaluation_cancelled");
        require(fingerprint() == context, "evaluation_configuration_changed");
        if(!job.flag("training", false)) verdict = gate_result(report);
        completed = true;
      } catch(const Error& e) { error = e.what(); }
      catch(...) { error = "agent_evaluation_failed"; }
      {
        std::lock_guard<std::mutex> guard(mutex);
        auto& current = state["jobs"][id]; current["finished_ms"] = now_ms();
        current["status"] = completed ? (verdict.flag("scored", false)
            ? (verdict.flag("passed", false) ? "passed" : "rejected") : "inconclusive") : "error";
        if(!error.empty()) current["error"] = error;
        if(completed && job.flag("training", false)) current["status"] = "completed";
        if(completed && !job.flag("training", false)) {
          if(job.flag("gepa", false)) {
            current["search"] = job["search"];
            current["version"] = agent_version_id(artifact.dump());
            write(root / id / "candidate.json", artifact.dump());
          }
          current["gate"] = verdict;
          // Search mutations do not rewrite memory or create per-memory evidence.
          if(verdict.flag("scored", false) && !job.flag("gepa", false)) record_feedback(id, current, artifact, base, verdict, report);
          const auto key = lane(job.str("user"), artifact.str("level"));
          const auto previous = state["publications"][key].str("context") == context
              ? state["publications"][key].str("version") : "";
          if(verdict.flag("passed", false) && state["latest"].str(key) == id && previous == job.str("parent_version")) {
            state["publications"][key] = Json{{"user", job["user"]}, {"level", artifact["level"]}, {"job_id", id}, {"version", current["version"]}, {"context", context}};
            // Persist before making the new snapshot available to consumers.
            save(); published[key] = artifact; current["published"] = true;
          }
          if(job.flag("gepa", false)) {
            write(root / id / "search/decision.json", Json{{"gate", verdict}, {"published", current.flag("published", false)},
                {"version", current["version"]}, {"parent_version", current["parent_version"]}, {"context", context}}.dump());
            if(current.flag("published", false)) write(root / id / "search/published-version.json",
                Json{{"schema_version", 1}, {"version", current["version"]}, {"artifact", artifact},
                    {"evaluation", Json{{"job_id", id}, {"context", context}, {"gate", verdict}}}}.dump());
          }
        }
        save();
      }
      if(changed) changed();
    }
  }
};

AgentEvaluationService::AgentEvaluationService(const fs::path& file, const fs::path& root, std::function<void()> changed, std::string controls)
    : impl_(std::make_unique<Impl>(file, root, std::move(changed), controls)) {
  impl_->worker = std::thread([this] {
    try { impl_->loop(); }
    catch(...) {
      { std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->state["worker_error"] = "evaluation_worker_failed";
        try { impl_->save(); } catch(...) {} }
      if(impl_->changed) impl_->changed();
    }
  });
}
AgentEvaluationService::~AgentEvaluationService() {
  impl_->stopping = true; impl_->wake.notify_all(); if(impl_->worker.joinable()) impl_->worker.join();
}
std::string AgentEvaluationService::agent_configuration() const { return impl_->config["agent"].dump(); }
std::string AgentEvaluationService::prepare_training(const std::string& user,
    const std::string& seed_json, bool learning_pending, bool learning_failed) {
  if(impl_->gepa.null() || !impl_->config["training"].flag("enabled", true))
    return Json{{"status", "disabled"}}.dump();
  const auto seed = Json::parse(seed_json);
  require(seed.str("user_id") == user && seed["agent"].dump() == impl_->config["agent"].dump(),
      "evaluation_owner_or_agent_mismatch", 403);
  const auto usable = memories(seed).fields().size();
  const auto& settings = impl_->config["training"];
  const auto minimum = settings.num("min_experiences", 3);
  const auto data = read_evaluation_dataset(impl_->dataset_path);
  for(const auto& issue : data.issues()) require(issue.severity() != "error", "training_dataset_invalid");
  // Memory counters are deliberately absent: learning must not create a fresh
  // training budget for the same frozen application and dataset.
  const auto key = lane(user, "training");
  const auto identity = digest(impl_->config["agent"].dump() + '\n' + data.dataset_version());
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(!impl_->state.has("worker_error"), "evaluation_worker_unavailable", 503);
  auto& progress = impl_->state["training"][key];
  const auto previous = progress.dump();
  const bool new_identity = progress.str("identity") != identity;
  if(new_identity)
    progress = Json{{"identity", identity}, {"user", user}, {"batches", 0}, {"tasks", Json::Array{}}};
  progress["usable_experiences"] = usable; progress["min_experiences"] = minimum;
  auto finish = [&](const std::string& status) {
    progress["status"] = status;
    const auto bytes = progress.dump();
    if(bytes != previous) impl_->save();
    return bytes;
  };
  if(learning_failed && !new_identity) return finish("learning_failed");
  const auto id = progress.str("job_id");
  if(!id.empty()) {
    const auto& job = impl_->state["jobs"][id];
    require(job.flag("training", false) && job.str("user") == user, "training_job_missing");
    const auto status = job.str("status");
    if(job.str("context") != impl_->context
        && (status == "superseded" || status == "interrupted" || status == "error")
        && !job.flag("cancel_requested", false) && job.str("error") != "evaluation_cancelled"
        && !job.has("training_import")) {
      // Requeue the same reserved batch under the new context. Never reuse its
      // old checkpoint or reset the dataset's task/batch budget.
      const auto tasks = job["task_ids"], artifact = baseline(seed);
      require(!tasks.items().empty(), "training_tasks_missing");
      const auto next = digest(id + '\n' + impl_->context);
      impl_->make_room();
      write(impl_->root / next / "candidate.json", artifact.dump());
      write(impl_->root / next / "baseline.json", artifact.dump());
      impl_->state["jobs"][next] = Json{{"user", user}, {"level", "L0"}, {"version", agent_version_id(artifact.dump())},
          {"status", "pending"}, {"queued_ms", now_ms()}, {"context", impl_->context}, {"attempt", 0},
          {"training", true}, {"task_ids", tasks}, {"supersedes", id}};
      impl_->state["jobs"][id]["status"] = "superseded";
      progress["job_id"] = next; progress["run_ids"] = Json::Array{};
      const auto result = finish("training"); impl_->wake.notify_all(); return result;
    }
    if(job.str("status") == "pending" || job.str("status") == "running") return finish("training");
    if(job.str("status") != "completed") return finish("training_failed");
    if(!job.has("training_import")) {
      auto result = Json::parse(finish("ready_to_import"));
      result["report_directory"] = (impl_->root / id / ("attempt-" + std::to_string(job.num("attempt", 0)))).string();
      result["dataset_directory"] = impl_->dataset_path.string(); result["baseline"] = impl_->load_artifact(id);
      return result.dump();
    }
    if(!job["training_import"].flag("ok", false)) return finish("import_failed");
  }
  if(learning_pending) return finish("waiting_for_learning");
  if(usable >= minimum && seed.str("level") != "L0") return finish("ready");
  if(progress.num("batches", 0) >= settings.num("max_batches", 4)) return finish("budget_exhausted");
  std::set<std::string> used;
  for(const auto& task : progress["tasks"].items()) used.insert(task.text());
  Json tasks = Json::Array{}, versions = Json::Array{};
  for(const auto& task : data.tasks()) {
    if(task.split() != "train" || task.review_status() != "reviewed" || used.count(task.task_version())) continue;
    if(tasks.items().size() >= settings.num("batch_size", 16)) break;
    tasks.push(task.task_id()); used.insert(task.task_version()); versions.push(task.task_version());
  }
  if(tasks.items().empty()) return finish("training_exhausted");
  const auto artifact = baseline(seed);
  const auto version = agent_version_id(artifact.dump());
  const auto job_id = digest(key + '\n' + identity + '\n' + tasks.dump());
  impl_->make_room();
  write(impl_->root / job_id / "candidate.json", artifact.dump());
  write(impl_->root / job_id / "baseline.json", artifact.dump());
  impl_->state["jobs"][job_id] = Json{{"user", user}, {"level", "L0"}, {"version", version},
      {"status", "pending"}, {"queued_ms", now_ms()}, {"context", impl_->context}, {"attempt", 0},
      {"training", true}, {"task_ids", tasks}};
  progress["batches"] = progress.num("batches", 0) + 1; progress["job_id"] = job_id;
  for(const auto& version : versions.items()) progress["tasks"].push(version);
  progress["run_ids"] = Json::Array{};
  const auto result = finish("training"); impl_->wake.notify_all(); return result;
}
void AgentEvaluationService::complete_training_import(const std::string& user,
    const std::string& id, const std::string& bytes) {
  const auto result = Json::parse(bytes); validate_local_data(result);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto& job = impl_->state["jobs"][id];
  require(job.flag("training", false) && job.str("user") == user && job.str("status") == "completed",
      "training_job_not_importable");
  job["training_import"] = result;
  impl_->state["training"][lane(user, "training")]["run_ids"] = result["result"].has("run_ids")
      ? result["result"]["run_ids"] : Json(Json::Array{});
  impl_->save();
}
std::string AgentEvaluationService::submit(const std::string& user, const std::string& bytes) {
  const auto version = agent_version_id(bytes); const auto artifact = Json::parse(bytes);
  require(artifact.str("user_id") == user && artifact["agent"].dump() == impl_->config["agent"].dump(), "evaluation_owner_or_agent_mismatch", 403);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(!impl_->state.has("worker_error"), "evaluation_worker_unavailable", 503);
  const auto key = lane(user, artifact.str("level"));
  const auto publication = impl_->state["publications"][key];
  const auto parent = publication.str("context") == impl_->context ? publication.str("version") : "";
  if(parent == version) return publication.str("job_id");
  const bool search = !impl_->gepa.null() && artifact.str("level") != "L0"
      && proto<v1::RuntimePolicy>(artifact["policy"]).options().approved_experiences_size() > 0;
  // A completed search must not schedule itself again when publication changes
  // the parent but the memory seed, dataset and configuration are unchanged.
  const auto id = digest(user + '\n' + version + '\n' + (search ? "gepa" : parent) + '\n' + impl_->context);
  if(impl_->state["jobs"].has(id)) return id;
  if(fs::exists(impl_->root / id / "job.json")) return id;
  impl_->make_room();
  Json base = impl_->published.has(key) ? impl_->published[key] : baseline(artifact);
  write(impl_->root / id / "candidate.json", bytes); write(impl_->root / id / "baseline.json", base.dump());
  if(search) write(impl_->root / id / "seed.json", bytes);
  for(auto& p : impl_->state["jobs"].fields()) if(p.second.str("user") == user && p.second.str("level") == artifact.str("level")
      && p.second.str("status") == "pending") p.second["status"] = "superseded";
  impl_->state["jobs"][id] = Json{{"user", user}, {"level", artifact["level"]}, {"version", version}, {"parent_version", parent},
      {"status", "pending"}, {"queued_ms", now_ms()}, {"context", impl_->context}, {"attempt", 0}, {"gepa", search}, {"seed_version", version}};
  impl_->state["latest"][key] = id; impl_->save(); impl_->wake.notify_all(); return id;
}
std::string AgentEvaluationService::status(const std::string& user) const {
  std::lock_guard<std::mutex> guard(impl_->mutex); Json result{{"jobs", Json::Object{}}, {"publications", Json::Object{}}};
  if(impl_->state.has("worker_error")) result["worker_error"] = impl_->state["worker_error"];
  if(impl_->state["training"].has(lane(user, "training")))
    result["training"] = impl_->state["training"][lane(user, "training")];
  for(const auto& p : impl_->state["jobs"].fields()) if(p.second.str("user") == user) result["jobs"][p.first] = p.second;
  for(const auto& p : impl_->state["publications"].fields()) if(p.second.str("user") == user && p.second.str("context") == impl_->context)
    result["publications"][p.second.str("level")] = p.second;
  return result.dump();
}
std::string AgentEvaluationService::publications(const std::string& user) const {
  std::lock_guard<std::mutex> guard(impl_->mutex); Json result = Json::Object{};
  for(const auto& p : impl_->published.fields()) if(p.second.str("user_id") == user) result[p.second.str("level")] = p.second;
  return result.dump();
}
std::string AgentEvaluationService::feedback(const std::string& user) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Json result = Json::Object{}; const auto& value = impl_->state["feedback"][user];
  if(value.object()) for(const auto& item : value.fields()) if(item.second.str("context") == impl_->context) result[item.first] = item.second;
  return result.dump();
}
std::string AgentEvaluationService::search_tree(const std::string& user, const std::string& id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(impl_->state["jobs"].has(id) && impl_->state["jobs"][id].str("user") == user, "evaluation_job_not_found", 404);
  require(impl_->state["jobs"][id].flag("gepa", false), "evaluation_job_not_search", 409);
  const auto path = impl_->root / id / "search/tree.json";
  return fs::exists(path) ? read(path, 16777216) : Json{{"status", "pending"}}.dump();
}
void AgentEvaluationService::retry(const std::string& user, const std::string& id) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(!impl_->state.has("worker_error"), "evaluation_worker_unavailable", 503);
  require(impl_->state["jobs"].has(id) && impl_->state["jobs"][id].str("user") == user, "evaluation_job_not_found", 404);
  auto& job = impl_->state["jobs"][id]; const auto status = job.str("status");
  require(status == "error" || status == "interrupted" || status == "inconclusive", "evaluation_job_not_retryable", 409);
  const auto latest = job.flag("training", false) ? impl_->state["training"][lane(user, "training")].str("job_id")
      : impl_->state["latest"].str(lane(user, job.str("level")));
  require(latest == id && job.str("context") == impl_->context, "evaluation_job_obsolete", 409);
  job["status"] = "pending"; job.fields().erase("error"); job.fields().erase("cancel_requested"); job["queued_ms"] = now_ms(); impl_->save(); impl_->wake.notify_all();
}
void AgentEvaluationService::cancel(const std::string& user, const std::string& id) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(impl_->state["jobs"].has(id) && impl_->state["jobs"][id].str("user") == user, "evaluation_job_not_found", 404);
  auto& job = impl_->state["jobs"][id];
  require(job.str("status") == "running" || job.str("status") == "pending", "evaluation_job_not_active", 409);
  if(job.str("status") == "running") { impl_->cancel_running = true; job["cancel_requested"] = true; }
  else { job["status"] = "error"; job["error"] = "evaluation_cancelled"; }
  impl_->save(); impl_->wake.notify_all();
}
}  // namespace trajectory::evolution
