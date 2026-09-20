#pragma once

// 搜索与评测共用执行、隐私和资源约束；不要在此绕过标准评测器。

#include "integration_json.h"
#include "trajectory_evolution/task_evaluation.h"

#include <cstddef>
#include <vector>

namespace trajectory::evolution::integration {

// 仅在既有评测工作线程中运行。宿主执行、评分、隐私检查和资源限制
// 与普通策略评测完全共用。每个最终候选保留其验证排名；调用方按此顺序
// 应用部署门禁，而不是按留出分数重新挑选最高者。
struct GepaFinalist {
  std::size_t node = 0;
  Json artifact;
  dataset::v1::TaskEvaluationReport holdout;
  Json control_coverage;
};

struct GepaResult {
  Json artifact;
  dataset::v1::TaskEvaluationReport holdout;
  Json summary;
  std::vector<GepaFinalist> finalists;
};

Json load_gepa_config(const std::filesystem::path& path);
std::string gepa_config_fingerprint(const Json& config);
GepaResult run_gepa_search(const Json& seed, const Json& baseline,
    const std::filesystem::path& dataset, const std::filesystem::path& evaluation_path,
    const Json& evaluation, const Json& config, const Json& gate, const Json& control_learning,
    const std::filesystem::path& output, const std::string& context, const std::string& evaluation_context,
    const std::string& candidate_evaluation_context,
    const std::function<bool()>& cancelled,
    std::shared_ptr<agentflow::trajectory::ModelAdapter> reflection_model = nullptr,
    std::size_t final_holdout_top_k_override = 0,
    bool final_holdout_replay = false);

}  // namespace trajectory::evolution::integration
