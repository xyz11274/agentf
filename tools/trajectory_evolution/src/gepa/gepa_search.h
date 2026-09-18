#pragma once

// 搜索与评测共用执行、隐私和资源约束；不要在此绕过标准评测器。

#include "integration_json.h"
#include "trajectory_evolution/task_evaluation.h"

namespace trajectory::evolution::integration {

// Runs exclusively in the existing evaluation worker. Host execution, grading,
// privacy checks and resource limits are shared with ordinary policy evaluation.
struct GepaResult {
  Json artifact;
  dataset::v1::TaskEvaluationReport holdout;
  Json summary;
};

Json load_gepa_config(const std::filesystem::path& path);
std::string gepa_config_fingerprint(const Json& config);
GepaResult run_gepa_search(const Json& seed, const Json& baseline,
    const std::filesystem::path& dataset, const std::filesystem::path& evaluation_path,
    const Json& evaluation, const Json& config, const Json& gate, const Json& control_learning,
    const std::filesystem::path& output, const std::string& context, const std::string& evaluation_context,
    const std::function<bool()>& cancelled,
    std::shared_ptr<agentflow::trajectory::ModelAdapter> reflection_model = nullptr);

}  // namespace trajectory::evolution::integration
