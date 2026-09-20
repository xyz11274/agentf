#pragma once

#include "local_support.h"
#include "trajectory_evolution/task_evaluation.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace trajectory::evolution::integration {

// 只复用留出集的固定 L0，不复用旧候选；文件读写沿用 GEPA 的安全路径和原子写入。
class HoldoutBaselineCache {
 public:
  using Report = dataset::v1::TaskEvaluationReport;
  using Path = std::filesystem::path;
  Path root;
  Json identity;
  std::string legacy_key;
  std::size_t artifact_limit;
  std::function<std::string(const Path&, std::size_t)> read;
  std::function<void(const Path&, const std::string&)> write;
  std::function<std::string(const std::string&)> hash;

  Path index_path() const { return root / "holdout-baseline-cache" / (hash(identity.dump()) + ".json"); }
  Path source_path(const Json& source) const {
    const Path relative(source.str("report"));
    require(!relative.empty() && !relative.is_absolute() && relative.filename() == "report.json",
        "gepa_holdout_cache_path_invalid");
    for(const auto& part : relative) require(part != "." && part != "..", "gepa_holdout_cache_path_invalid");
    return root / relative;
  }
  void artifacts(const Report& report, const Path& source, const Path& destination = {}) const {
    for(const auto& trial : report.trials()) {
      require(Path(trial.trial_id()).filename() == trial.trial_id() && trial.trial_id() != "."
          && trial.trial_id() != ".." && !trial.trial_id().empty(), "gepa_holdout_cache_artifact_path_invalid");
      const auto evidence = json(trial.evidence()); std::size_t total = 0;
      if(evidence["artifacts"].object()) for(const auto& item : evidence["artifacts"].fields()) {
        const auto name = item.second.str("stored_file");
        require(!name.empty() && Path(name).filename() == name && name != "." && name != "..",
            "gepa_holdout_cache_artifact_path_invalid");
        const auto bytes = read(source / "artifacts" / trial.trial_id() / name, artifact_limit);
        total += bytes.size();
        require(total <= artifact_limit && bytes.size() == item.second.num("size_bytes", SIZE_MAX)
            && hash(bytes) == item.second.str("sha256"), "gepa_holdout_cache_artifact_changed");
        if(!destination.empty()) write(destination / "artifacts" / trial.trial_id() / name, bytes);
      }
    }
  }
  Report load(const Json& source) const {
    require(identity.str("split") == "holdout" && source["identity"].dump() == identity.dump(),
        "gepa_holdout_cache_identity_changed");
    const auto path = source_path(source);
    const auto bytes = read(path, 134217728), manifest_bytes = read(path.parent_path() / "manifest.json", 16777216);
    require(hash(bytes) == source.str("report_hash") && hash(manifest_bytes) == source.str("manifest_hash"),
        "gepa_holdout_cache_source_changed");
    const auto manifest = Json::parse(manifest_bytes); const auto original = proto<Report>(Json::parse(bytes));
    require(original.complete() && manifest.flag("complete", false) && original.schema_version() == 1
        && original.report_kind() == "task_evaluation.v1" && original.evaluator_version() == "task-evaluator-v1"
        && original.dataset_version() == identity.str("dataset_version")
        && manifest.str("run_id") == original.run_id() && manifest["files"].str("report.json") == hash(bytes),
        "gepa_holdout_cache_report_invalid");
    // Manifest 中的原始输出文件都要校验；不能仅重算 JSON 摘要后接受被改写的记录。
    require(manifest["files"].object(), "gepa_holdout_cache_manifest_invalid");
    for(const auto& file : manifest["files"].fields()) {
      require(!file.first.empty() && Path(file.first).filename() == file.first && file.first != "." && file.first != "..",
          "gepa_holdout_cache_manifest_invalid");
      require(hash(read(path.parent_path() / file.first, 134217728)) == file.second.text(),
          "gepa_holdout_cache_source_changed");
    }
    const auto configuration = json(original.configuration());
    require(configuration["splits"].dump() == Json(Json::Array{"holdout"}).dump()
        && configuration.num("repetitions", 0) == identity.num("repetitions", 1),
        "gepa_holdout_cache_split_mismatch");
    bool version_seen = false;
    for(const auto& variant : configuration["variants"].items()) if(variant.str("id") == "baseline") {
      require(!version_seen && variant.str("version") == identity.str("baseline_version"),
          "gepa_holdout_cache_version_mismatch"); version_seen = true;
    }
    require(version_seen, "gepa_holdout_cache_version_mismatch");
    Report baseline = original; baseline.clear_trials();
    std::set<std::pair<std::string, unsigned>> seen; std::set<std::string> trial_ids;
    for(const auto& trial : original.trials()) if(trial.variant_id() == "baseline") {
      const auto evidence = json(trial.evidence());
      require(trial.split() == "holdout" && identity["task_versions"].has(trial.task_id())
          && identity["task_versions"][trial.task_id()].text() == trial.task_version()
          && trial.repetition() > 0 && trial.repetition() <= identity.num("repetitions", 1)
          && seen.emplace(trial.task_id(), trial.repetition()).second && trial_ids.insert(trial.trial_id()).second
          && trial.error_code().empty() && (trial.execution_status() == "completed" || trial.execution_status() == "failed")
          && (trial.verdict() == "passed" || trial.verdict() == "failed")
          && evidence.str("loaded_version") == identity.str("baseline_version")
          && (!identity.flag("loop_review", false) || evidence["loop_review"].str("status") == "complete"),
          "gepa_holdout_cache_trial_mismatch");
      *baseline.add_trials() = trial;
    }
    require(seen.size() == identity["task_versions"].fields().size() * identity.num("repetitions", 1),
        "gepa_holdout_cache_trials_incomplete");
    const auto summary = json(original.summary())["by_variant"]["baseline"];
    require(summary.num("trials", 0) == seen.size() && summary.num("scored", 0) == seen.size(),
        "gepa_holdout_cache_summary_invalid");
    artifacts(baseline, path.parent_path());
    return baseline;
  }
  Json source(const Path& path, const std::string& report_hash) const {
    const auto relative = std::filesystem::absolute(path).lexically_normal().lexically_relative(
        std::filesystem::absolute(root).lexically_normal());
    source_path(Json{{"report", relative.string()}});
    const auto manifest = read(path.parent_path() / "manifest.json", 16777216);
    return Json{{"identity", identity}, {"report", relative.string()}, {"report_hash", report_hash},
        {"manifest_hash", hash(manifest)}, {"comparison_timing", "historical_baseline"}};
  }
  Json lookup(const Json& pinned = {}) const {
    Json selected;
    try {
      if(pinned.has("source")) selected = pinned["source"];
      else if(std::filesystem::exists(index_path())) selected = Json::parse(read(index_path(), 16384));
      else {
        // 旧版通过 val L0 的身份链证明执行条件；这里只读取旧最终报告的 holdout baseline。
        // 不看分数选来源：按作业开始时间、路径固定选择最早的兼容来源，选中后损坏即失效。
        const auto legacy = Json::parse(read(root / "baseline-cache" / (legacy_key + ".json"), 1048576));
        require(legacy["identity"].str("evaluation_context") == identity.str("evaluation_context")
            && legacy["identity"].str("baseline_version") == identity.str("baseline_version")
            && legacy["identity"].str("dataset_version") == identity.str("dataset_version")
            && hash(legacy["identity"].dump()) == legacy_key, "gepa_holdout_cache_legacy_identity_changed");
        std::vector<std::tuple<std::size_t, std::string, Json>> candidates;
        for(const auto& directory : std::filesystem::directory_iterator(root)) {
          if(!directory.is_directory() || directory.is_symlink()) continue;
          const auto path = directory.path() / "search/tree.json";
          if(!std::filesystem::exists(path)) continue;
          Json previous;
          try { previous = Json::parse(read(path, 16777216)); } catch(const std::exception&) { continue; }
          if(previous["baseline_cache"].str("key") != legacy_key
              || previous.str("baseline_version") != identity.str("baseline_version")
              || previous.str("dataset_version") != identity.str("dataset_version")) continue;
          for(const auto& item : previous["evaluations"].fields()) {
            const auto& row = item.second;
            if(row.str("split") != "holdout" || row.str("status") != "complete"
                || row.str("baseline_version") != identity.str("baseline_version")) continue;
            const auto report = directory.path() / "search" / row.str("report");
            candidates.emplace_back(previous.num("started_ms", SIZE_MAX), report.string(), row);
          }
        }
        if(candidates.empty()) return Json{{"status", "miss"}, {"reason", "no_compatible_holdout_baseline"}};
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
          return std::tie(std::get<0>(a), std::get<1>(a)) < std::tie(std::get<0>(b), std::get<1>(b));
        });
        const auto& first = candidates.front();
        selected = source(std::get<1>(first), std::get<2>(first).str("report_hash"));
      }
      load(selected);
      if(!std::filesystem::exists(index_path())) write(index_path(), selected.dump());
      return Json{{"status", "hit"}, {"source", selected}};
    } catch(const std::exception& error) {
      // 只给出稳定错误码；不暴露 filesystem_error 中可能包含的本地路径。
      const std::string code = error.what();
      return Json{{"status", "invalid"}, {"reason", code.rfind("gepa_", 0) == 0 ? code : "gepa_holdout_cache_unavailable"}};
    }
  }
  void remember(const Path& path, const Report& report) const {
    if(std::filesystem::exists(index_path()) || !report.complete()) return;
    const auto selected = source(path, hash(read(path, 134217728))); load(selected);
    write(index_path(), selected.dump());
  }
  Report combine(const Json& selected, const Report& candidate, const Path& candidate_path,
      const Path& destination, const std::string& key, std::size_t report_limit = 134217728) const {
    const auto baseline = load(selected);
    const auto fresh_bytes = read(candidate_path, 134217728);
    const auto fresh_manifest = Json::parse(read(candidate_path.parent_path() / "manifest.json", 16777216));
    require(fresh_manifest.str("run_id") == candidate.run_id()
        && fresh_manifest["files"].str("report.json") == hash(fresh_bytes)
        && json(proto<Report>(Json::parse(fresh_bytes))).dump() == json(candidate).dump()
        && candidate.dataset_version() == baseline.dataset_version()
        && candidate.evaluator_version() == baseline.evaluator_version(), "gepa_holdout_candidate_changed");
    const auto config = json(candidate.configuration());
    require(config["splits"].dump() == Json(Json::Array{"holdout"}).dump()
        && config.num("repetitions", 0) == identity.num("repetitions", 1)
        && config["variants"].array() && config["variants"].items().size() == 1
        && config["variants"].items()[0].str("id") == "candidate", "gepa_holdout_cache_split_mismatch");
    std::map<std::pair<std::string, unsigned>, const dataset::v1::EvaluationTrial*> bases;
    std::set<std::string> trial_ids; std::set<std::pair<std::string, unsigned>> seen;
    for(const auto& trial : baseline.trials()) {
      bases[{trial.task_id(), trial.repetition()}] = &trial; trial_ids.insert(trial.trial_id());
    }
    std::size_t pairs = 0, excluded = 0, wins = 0, losses = 0;
    for(const auto& trial : candidate.trials()) {
      const auto id = std::make_pair(trial.task_id(), trial.repetition());
      require(trial.variant_id() == "candidate" && trial.split() == "holdout" && bases.count(id)
          && seen.insert(id).second && trial_ids.insert(trial.trial_id()).second
          && trial.task_version() == bases.at(id)->task_version()
          && json(trial.evidence()).str("loaded_version") == config["variants"].items()[0].str("version"),
          "gepa_holdout_candidate_trial_mismatch");
      if(trial.verdict() != "passed" && trial.verdict() != "failed") { ++excluded; continue; }
      ++pairs; wins += trial.verdict() == "passed" && bases.at(id)->verdict() == "failed";
      losses += trial.verdict() == "failed" && bases.at(id)->verdict() == "passed";
    }
    Report combined = candidate;
    for(const auto& trial : baseline.trials()) *combined.add_trials() = trial;
    combined.set_report_kind("gepa_cached_pair.v1"); combined.set_run_id("gepa-holdout-" + key);
    combined.set_config_fingerprint(hash(candidate.config_fingerprint() + selected.dump()));
    combined.set_complete(candidate.complete() && seen.size() == bases.size());
    const auto candidate_summary = json(candidate.summary());
    Json summary{{"trials", std::size_t(combined.trials_size())}, {"planned_trials", bases.size() * 2},
        {"not_started_trials", bases.size() - seen.size()}, {"reused_baseline_trials", bases.size()},
        {"model_calls", candidate_summary["model_calls"]}, {"comparison_timing", "historical_baseline"},
        {"baseline_source", selected}, {"candidate_source", Json{{"run_id", candidate.run_id()},
            {"report_hash", hash(fresh_bytes)}}}, {"by_variant", Json{
            {"baseline", json(baseline.summary())["by_variant"]["baseline"]},
            {"candidate", candidate_summary["by_variant"]["candidate"]}}}};
    *combined.mutable_summary() = proto<google::protobuf::Struct>(summary);
    *combined.mutable_comparisons() = proto<google::protobuf::Struct>(Json{{"candidate", Json{
        {"baseline", "baseline"}, {"paired_trials", pairs}, {"excluded_pairs", excluded}, {"wins", wins},
        {"losses", losses}, {"ties", pairs - wins - losses}, {"pass_rate_delta", pairs
            ? Json((double(wins) - double(losses)) / pairs) : Json()}, {"automatic_promotion", false},
        {"comparison_timing", "historical_baseline"}}}});
    auto configuration = config;
    configuration["variants"].items().insert(configuration["variants"].items().begin(),
        Json{{"id", "baseline"}, {"version", identity["baseline_version"]}});
    configuration["comparison_timing"] = "historical_baseline";
    *combined.mutable_configuration() = proto<google::protobuf::Struct>(configuration);
    const auto bytes = json(combined).dump(), pb = combined.SerializeAsString();
    // 配对报告仍服从原批次的总量上限，拒绝时不留下半套产物。
    require(bytes.size() <= report_limit && pb.size() <= report_limit, "evaluation_report_size_limit");
    artifacts(baseline, source_path(selected).parent_path(), destination);
    artifacts(candidate, candidate_path.parent_path(), destination);
    write(destination / "report.json", bytes); write(destination / "report.pb", pb);
    write(destination / "manifest.json", Json{{"complete", combined.complete()}, {"run_id", combined.run_id()},
        {"dataset_version", combined.dataset_version()}, {"summary", summary},
        {"files", Json{{"report.json", hash(bytes)}, {"report.pb", hash(pb)}}}}.dump());
    return combined;
  }
};
}  // namespace trajectory::evolution::integration
