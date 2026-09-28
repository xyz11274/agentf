// 留出基线缓存：复用同数据、同宿主、同评分口径的固定 L0 结果。
// 读取时验证清单、内容哈希、版本与产物，不仅检查文件是否存在。
// 只复用基线，不用旧候选替代新候选的真实评测；新旧评测版本必须隔离。

#pragma once

#include "local_support.h"
#include "trajectory_evolution/task_evaluation.h"
#include "goal_assessment.h"
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
  std::function<std::string(const Path &, std::size_t)> read;
  std::function<void(const Path &, const std::string &)> write;
  std::function<std::string(const std::string &)> hash;

  // 按缓存身份生成索引路径，同一数据与评测口径才能定位同一份缓存。
  Path index_path() const {
    return root / "holdout-baseline-cache" / (hash(identity.dump()) + ".json");
  }

  // 解析缓存记录中的相对产物路径，拒绝绝对路径和父目录跳转。
  Path source_path(const Json &source) const {
    const Path relative(source.str("report"));
    require(!relative.empty() && !relative.is_absolute() && relative.filename() == "report.json",
            "gepa_holdout_cache_path_invalid");
    for (const auto &part : relative)
      require(part != "." && part != "..", "gepa_holdout_cache_path_invalid");
    return root / relative;
  }

  // 校验当前报告引用的产物大小、哈希和归属，必要时复制到新结果目录。
  void artifacts(const Report &report, const Path &source, const Path &destination = {}) const {
    for (const auto &trial : report.trials()) {
      require(Path(trial.trial_id()).filename() == trial.trial_id() && trial.trial_id() != "." &&
                  trial.trial_id() != ".." && !trial.trial_id().empty(),
              "gepa_holdout_cache_artifact_path_invalid");
      const auto evidence = json(trial.evidence());
      std::size_t total = 0;
      if (evidence["artifacts"].object())
        for (const auto &item : evidence["artifacts"].fields()) {
          const auto name = item.second.str("stored_file");
          require(!name.empty() && Path(name).filename() == name && name != "." && name != "..",
                  "gepa_holdout_cache_artifact_path_invalid");
          const auto bytes = read(source / "artifacts" / trial.trial_id() / name, artifact_limit);
          total += bytes.size();
          require(total <= artifact_limit &&
                      bytes.size() == item.second.num("size_bytes", SIZE_MAX) &&
                      hash(bytes) == item.second.str("sha256"),
                  "gepa_holdout_cache_artifact_changed");
          if (!destination.empty())
            write(destination / "artifacts" / trial.trial_id() / name, bytes);
        }
    }
  }

  // 载入并验证持久化内容，拒绝版本、摘要或约束不匹配的结果。
  Report load(const Json &source) const {
    // 基线只绑定数据集、评分口径、L0 版本和任务版本；候选/管理器上下文
    // 不参与 L0 身份。兼容旧索引时忽略历史记录中残留的 candidate 字段，
    // 避免管理器重编译后把同一份固定基线误判为损坏。
    auto cached_identity = source["identity"];
    cached_identity.fields().erase("candidate_evaluation_context");
    auto expected_identity = identity;
    expected_identity.fields().erase("candidate_evaluation_context");
    require(identity.str("split") == "holdout" &&
                cached_identity.dump() == expected_identity.dump(),
            "gepa_holdout_cache_identity_changed");
    std::cerr << "[gepa] holdout_cache_stage=identity\n";
    const auto path = source_path(source);
    // 报告 JSON 可能达到数百 MB。这里只校验哈希，不再把它解析成
    // RapidJSON 树；真正的报告对象从同目录的 protobuf 直接恢复，
    // 避免一次恢复同时保留 JSON 原文、JSON 树和 protobuf 三份内存。
    const auto report_json_hash = hash(read(path, kMaxEvaluationReportBytes));
    const auto manifest_bytes = read(path.parent_path() / "manifest.json", 16777216);
    require(report_json_hash == source.str("report_hash") &&
                hash(manifest_bytes) == source.str("manifest_hash"),
            "gepa_holdout_cache_source_changed");
    const auto manifest = Json::parse(manifest_bytes);
    Report original;
    std::string report_pb_hash;
    {
      const auto report_pb = read(path.parent_path() / "report.pb", kMaxEvaluationReportBytes);
      report_pb_hash = hash(report_pb);
      require(manifest["files"].str("report.pb") == report_pb_hash &&
                  original.ParseFromString(report_pb),
              "gepa_holdout_cache_report_invalid");
    }
    require(original.complete() && manifest.flag("complete", false) &&
                original.schema_version() == 1 && original.report_kind() == "task_evaluation.v1" &&
                original.evaluator_version() ==
                    identity.str("evaluator_version", "task-evaluator-v1") &&
                original.dataset_version() == identity.str("dataset_version") &&
                manifest.str("run_id") == original.run_id() &&
                manifest["files"].str("report.json") == report_json_hash &&
                manifest["files"].str("report.pb") == report_pb_hash,
            "gepa_holdout_cache_report_invalid");
    std::cerr << "[gepa] holdout_cache_stage=report\n";
    // Manifest 中的原始输出文件都要校验；不能仅重算 JSON 摘要后接受被改写的记录。
    require(manifest["files"].object(), "gepa_holdout_cache_manifest_invalid");
    for (const auto &file : manifest["files"].fields()) {
      require(!file.first.empty() && Path(file.first).filename() == file.first &&
                  file.first != "." && file.first != "..",
              "gepa_holdout_cache_manifest_invalid");
      // report.json/report.pb 已在上面分别校验，避免再把超大文件重复读入；
      // 其余产物继续逐项验证，保证缓存目录仍具备完整性保护。
      if (file.first == "report.json")
        require(file.second.text() == report_json_hash, "gepa_holdout_cache_source_changed");
      else if (file.first == "report.pb")
        require(file.second.text() == report_pb_hash, "gepa_holdout_cache_source_changed");
      else
        require(hash(read(path.parent_path() / file.first, kMaxEvaluationReportBytes)) ==
                    file.second.text(),
                "gepa_holdout_cache_source_changed");
    }
    const auto configuration = json(original.configuration());
    std::cerr << "[gepa] holdout_cache_stage=configuration\n";
    require(configuration["splits"].dump() == Json(Json::Array{"holdout"}).dump() &&
                configuration.num("repetitions", 0) == identity.num("repetitions", 1),
            "gepa_holdout_cache_split_mismatch");
    bool version_seen = false;
    for (const auto &variant : configuration["variants"].items())
      if (variant.str("id") == "baseline") {
        require(!version_seen && variant.str("version") == identity.str("baseline_version"),
                "gepa_holdout_cache_version_mismatch");
        version_seen = true;
      }
    require(version_seen, "gepa_holdout_cache_version_mismatch");
    Report baseline = original;
    baseline.clear_trials();
    std::cerr << "[gepa] holdout_cache_stage=trials\n";
    std::set<std::pair<std::string, unsigned>> seen;
    std::set<std::string> trial_ids;
    for (const auto &trial : original.trials())
      if (trial.variant_id() == "baseline") {
        const auto evidence = json(trial.evidence());
        // 先验证完成度结构再读取数值，损坏的缓存不能在类型转换处绕过校验。
        if (evidence["assessment"].object())
          assessment_quality(evidence, trial.verdict() == "passed");
        require(
            trial.split() == "holdout" && identity["task_versions"].has(trial.task_id()) &&
                identity["task_versions"][trial.task_id()].text() == trial.task_version() &&
                trial.repetition() > 0 && trial.repetition() <= identity.num("repetitions", 1) &&
                seen.emplace(trial.task_id(), trial.repetition()).second &&
                trial_ids.insert(trial.trial_id()).second &&
                // 失败 trial 的 error_code 是业务轨迹的一部分，不能因为它存在
                // 就把整份可复用 L0 判成损坏；只有“已完成但带错误码”才是不一致。
                (trial.error_code().empty() || trial.execution_status() == "failed") &&
                (trial.execution_status() == "completed" || trial.execution_status() == "failed") &&
                (trial.verdict() == "passed" || trial.verdict() == "failed" ||
                 (trial.verdict() == "uncertain" && evidence["assessment"].object())) &&
                evidence.str("loaded_version") == identity.str("baseline_version") &&
                (!identity.flag("loop_review", false) ||
                 evidence["loop_review"].str("status") == "complete"),
            "gepa_holdout_cache_trial_mismatch");
        *baseline.add_trials() = trial;
      }
    require(seen.size() ==
                identity["task_versions"].fields().size() * identity.num("repetitions", 1),
            "gepa_holdout_cache_trials_incomplete");
    const auto summary = json(original.summary())["by_variant"]["baseline"];
    std::cerr << "[gepa] holdout_cache_stage=summary\n";
    const auto measured = original.evaluator_version() == "task-evaluator-v2-goals"
                              ? summary["goal_completion"].num("observations", 0)
                              : summary.num("scored", 0);
    require(summary.num("trials", 0) == seen.size() && measured == seen.size(),
            "gepa_holdout_cache_summary_invalid");
    artifacts(baseline, path.parent_path());
    std::cerr << "[gepa] holdout_cache_stage=artifacts\n";
    return baseline;
  }

  // 解析当前对象的来源，保留来源身份与后续引用所需的数据。
  Json source(const Path &path, const std::string &report_hash) const {
    const auto relative = std::filesystem::absolute(path).lexically_normal().lexically_relative(
        std::filesystem::absolute(root).lexically_normal());
    source_path(Json{{"report", relative.string()}});
    const auto manifest = read(path.parent_path() / "manifest.json", 16777216);
    return Json{{"identity", identity},
                {"report", relative.string()},
                {"report_hash", report_hash},
                {"manifest_hash", hash(manifest)},
                {"comparison_timing", "historical_baseline"}};
  }

  // 查找并验证可复用的缓存来源；未命中不意味着已完成评测。
  Json lookup(const Json &pinned = {}) const {
    Json selected;
    try {
      // 树状态在缓存失效时会保留 source:null；只有完整对象才是可加载来源，
      // 否则回退到同一身份的索引文件，避免把失效占位值传给 load()。
      if (pinned.object() && pinned.has("source") && pinned["source"].object())
        selected = pinned["source"];
      else if (std::filesystem::exists(index_path()))
        // 任务版本清单会随数据集增长，索引是元数据而非评测正文；
        // 16 KiB 会把合法的大数据集索引截断，导致 object_required。
        selected = Json::parse(read(index_path(), 4 * 1024 * 1024));
      else {
        // 旧版通过 val L0 的身份链证明执行条件；这里只读取旧最终报告的 holdout baseline。
        // 不看分数选来源：按作业开始时间、路径固定选择最早的兼容来源，选中后损坏即失效。
        const auto legacy =
            Json::parse(read(root / "baseline-cache" / (legacy_key + ".json"), 1048576));
        auto legacy_identity = legacy["identity"];
        legacy_identity.fields().erase("candidate_evaluation_context");
        auto expected_legacy_identity = identity;
        expected_legacy_identity.fields().erase("candidate_evaluation_context");
        require(
                legacy_identity.dump() == expected_legacy_identity.dump() &&
                legacy["identity"].str("baseline_version") == identity.str("baseline_version") &&
                legacy["identity"].str("dataset_version") == identity.str("dataset_version") &&
                hash(legacy["identity"].dump()) == legacy_key,
            "gepa_holdout_cache_legacy_identity_changed");
        std::vector<std::tuple<std::size_t, std::string, Json>> candidates;
        for (const auto &directory : std::filesystem::directory_iterator(root)) {
          if (!directory.is_directory() || directory.is_symlink())
            continue;
          const auto path = directory.path() / "search/tree.json";
          if (!std::filesystem::exists(path))
            continue;
          Json previous;
          try {
            previous = Json::parse(read(path, 16777216));
          } catch (const std::exception &) {
            continue;
          }
          if (previous["baseline_cache"].str("key") != legacy_key ||
              previous.str("baseline_version") != identity.str("baseline_version") ||
              previous.str("dataset_version") != identity.str("dataset_version"))
            continue;
          for (const auto &item : previous["evaluations"].fields()) {
            const auto &row = item.second;
            if (row.str("split") != "holdout" || row.str("status") != "complete" ||
                row.str("baseline_version") != identity.str("baseline_version"))
              continue;
            const auto report = directory.path() / "search" / row.str("report");
            candidates.emplace_back(previous.num("started_ms", SIZE_MAX), report.string(), row);
          }
        }
        if (candidates.empty())
          return Json{{"status", "miss"}, {"reason", "no_compatible_holdout_baseline"}};
        std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
          return std::tie(std::get<0>(a), std::get<1>(a)) <
                 std::tie(std::get<0>(b), std::get<1>(b));
        });
        const auto &first = candidates.front();
        selected = source(std::get<1>(first), std::get<2>(first).str("report_hash"));
      }
      load(selected);
      if (!std::filesystem::exists(index_path()))
        write(index_path(), selected.dump());
      return Json{{"status", "hit"}, {"source", selected}};
    } catch (const std::exception &error) {
      // 只给出稳定错误码；不暴露 filesystem_error 中可能包含的本地路径。
      const std::string code = error.what();
      // 记录稳定错误码，便于定位缓存未命中的具体校验阶段；不打印本地路径、
      // 评测内容或凭据。外部状态仍只返回稳定的 invalid 原因。
      std::cerr << "[gepa] holdout_cache_lookup_failed code=" << code << '\n';
      return Json{
          {"status", "invalid"},
          {"reason", code.rfind("gepa_", 0) == 0 ? code : "gepa_holdout_cache_unavailable"}};
    }
  }

  // 登记已完成且通过完整性校验的缓存来源，供后续固定基线复用。
  void remember(const Path &path, const Report &report) const {
    if (std::filesystem::exists(index_path()) || !report.complete())
      return;
    const auto selected = source(path, hash(read(path, kMaxEvaluationReportBytes)));
    load(selected);
    write(index_path(), selected.dump());
  }

  // 合并目标集合对应的缓存结果，校验任务与版本是否完整匹配。
  Report combine(const Json &selected, const Report &candidate, const Path &candidate_path,
                 const Path &destination, const std::string &key,
                 std::size_t report_limit = kMaxEvaluationReportBytes) const {
    const auto baseline = load(selected);
    const auto fresh_bytes = read(candidate_path, kMaxEvaluationReportBytes);
    const auto fresh_manifest =
        Json::parse(read(candidate_path.parent_path() / "manifest.json", 16777216));
    require(fresh_manifest.str("run_id") == candidate.run_id() &&
                fresh_manifest["files"].str("report.json") == hash(fresh_bytes) &&
                json(proto<Report>(Json::parse(fresh_bytes))).dump() == json(candidate).dump() &&
                candidate.dataset_version() == baseline.dataset_version() &&
                candidate.evaluator_version() == baseline.evaluator_version(),
            "gepa_holdout_candidate_changed");
    const auto config = json(candidate.configuration());
    require(config["splits"].dump() == Json(Json::Array{"holdout"}).dump() &&
                config.num("repetitions", 0) == identity.num("repetitions", 1) &&
                config["variants"].array() && config["variants"].items().size() == 1 &&
                config["variants"].items()[0].str("id") == "candidate",
            "gepa_holdout_cache_split_mismatch");
    std::map<std::pair<std::string, unsigned>, const dataset::v1::EvaluationTrial *> bases;
    std::set<std::string> trial_ids;
    std::set<std::pair<std::string, unsigned>> seen;
    for (const auto &trial : baseline.trials()) {
      bases[{trial.task_id(), trial.repetition()}] = &trial;
      trial_ids.insert(trial.trial_id());
    }
    std::size_t pairs = 0, excluded = 0, wins = 0, losses = 0;
    // The strict verdict pairing above is deliberately kept separate from the
    // objective-quality pairing.  In particular, a cached pair report must not
    // claim that its top-level paired_trials also covers goal completion: a
    // valid objective assessment can exist on a trial whose strict verdict is
    // uncertain or failed.
    std::size_t objective_pairs = 0, objective_wins = 0, objective_losses = 0;
    double objective_delta = 0;
    auto objective_quality = [](const auto &trial, double &quality) {
      if (trial.verdict() == "error" || !trial.error_code().empty())
        return false;
      try {
        const auto evidence = json(trial.evidence());
        if (!evidence["assessment"].object())
          return false;
        quality = assessment_quality(evidence, trial.verdict() == "passed");
        for (const auto &check : trial.checks())
          if (check.check_id() != "model" && check.status() == "failed")
            quality = 0;
        return true;
      } catch (...) {
        return false;
      }
    };
    for (const auto &trial : candidate.trials()) {
      const auto id = std::make_pair(trial.task_id(), trial.repetition());
      require(trial.variant_id() == "candidate" && trial.split() == "holdout" && bases.count(id) &&
                  seen.insert(id).second && trial_ids.insert(trial.trial_id()).second &&
                  trial.task_version() == bases.at(id)->task_version() &&
                  json(trial.evidence()).str("loaded_version") ==
                      config["variants"].items()[0].str("version"),
              "gepa_holdout_candidate_trial_mismatch");
      double candidate_quality = 0, baseline_quality = 0;
      if (objective_quality(trial, candidate_quality) &&
          objective_quality(*bases.at(id), baseline_quality)) {
        const auto delta = candidate_quality - baseline_quality;
        ++objective_pairs;
        objective_delta += delta;
        objective_wins += delta > 1e-12;
        objective_losses += delta < -1e-12;
      }
      if (trial.verdict() != "passed" && trial.verdict() != "failed") {
        ++excluded;
        continue;
      }
      ++pairs;
      wins += trial.verdict() == "passed" && bases.at(id)->verdict() == "failed";
      losses += trial.verdict() == "failed" && bases.at(id)->verdict() == "passed";
    }
    Report combined = candidate;
    for (const auto &trial : baseline.trials())
      *combined.add_trials() = trial;
    combined.set_report_kind("gepa_cached_pair.v1");
    combined.set_run_id("gepa-holdout-" + key);
    combined.set_config_fingerprint(hash(candidate.config_fingerprint() + selected.dump()));
    combined.set_complete(candidate.complete() && seen.size() == bases.size());
    const auto candidate_summary = json(candidate.summary());
    Json summary{
        {"trials", std::size_t(combined.trials_size())},
        {"planned_trials", bases.size() * 2},
        {"not_started_trials", bases.size() - seen.size()},
        {"reused_baseline_trials", bases.size()},
        {"model_calls", candidate_summary["model_calls"]},
        {"comparison_timing", "historical_baseline"},
        {"baseline_source", selected},
        {"candidate_source",
         Json{{"run_id", candidate.run_id()}, {"report_hash", hash(fresh_bytes)}}},
        {"by_variant", Json{{"baseline", json(baseline.summary())["by_variant"]["baseline"]},
                            {"candidate", candidate_summary["by_variant"]["candidate"]}}}};
    *combined.mutable_summary() = proto<google::protobuf::Struct>(summary);
    *combined.mutable_comparisons() = proto<google::protobuf::Struct>(Json{
        {"candidate",
         Json{{"baseline", "baseline"},
              {"paired_trials", pairs},
              {"excluded_pairs", excluded},
              {"wins", wins},
              {"losses", losses},
              {"ties", pairs - wins - losses},
              {"pass_rate_delta", pairs ? Json((double(wins) - double(losses)) / pairs) : Json()},
              {"goal_completion",
               Json{{"paired_trials", objective_pairs},
                    {"wins", objective_wins},
                    {"losses", objective_losses},
                    {"ties", objective_pairs - objective_wins - objective_losses},
                    {"quality_delta", objective_pairs ? Json(objective_delta / objective_pairs)
                                                        : Json()} }},
              {"automatic_promotion", false},
              {"comparison_timing", "historical_baseline"}}}});
    auto configuration = config;
    configuration["variants"].items().insert(
        configuration["variants"].items().begin(),
        Json{{"id", "baseline"}, {"version", identity["baseline_version"]}});
    configuration["comparison_timing"] = "historical_baseline";
    *combined.mutable_configuration() = proto<google::protobuf::Struct>(configuration);
    const auto bytes = json(combined).dump(), pb = combined.SerializeAsString();
    // 配对报告仍服从原批次的总量上限，拒绝时不留下半套产物。
    require(bytes.size() <= report_limit && pb.size() <= report_limit,
            "evaluation_report_size_limit");
    artifacts(baseline, source_path(selected).parent_path(), destination);
    artifacts(candidate, candidate_path.parent_path(), destination);
    write(destination / "report.json", bytes);
    write(destination / "report.pb", pb);
    write(destination / "manifest.json",
          Json{{"complete", combined.complete()},
               {"run_id", combined.run_id()},
               {"dataset_version", combined.dataset_version()},
               {"summary", summary},
               {"files", Json{{"report.json", hash(bytes)}, {"report.pb", hash(pb)}}}}
              .dump());
    return combined;
  }
};
} // 命名空间结束：trajectory::evolution::integration
