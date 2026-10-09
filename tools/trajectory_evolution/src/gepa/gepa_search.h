// GEPA 搜索主流程：生成候选、选择父节点、融合分支并调度训练和验证。
// 候选是持久化策略版本，不直接修改宿主代码；实际效果由统一评测器测量。
// 缓存身份必须包含数据集、运行版本和评分语义，不能把旧评分与新评分直接混用。
// 留出数据用于最终比较，不作为反思输入或下一代候选的训练证据。

#pragma once

// 搜索与评测共用执行、隐私和资源约束；不要在此绕过标准评测器。

#include "integration_json.h"
#include "trajectory_evolution/task_evaluation.h"

#include <cstddef>
#include <vector>

namespace trajectory::evolution::integration {

// 只认本树已持久化的真实旧预算；已留出、缺基线或未扩调用上限都不可重开。
inline bool gepa_model_budget_extended(const Json &tree, const Json &config) {
  const auto &history = tree["budget_extensions"];
  if (tree.str("status") != "validation_rejected" ||
      tree.str("stop_reason") != "model_budget" || tree.flag("holdout_started", false) ||
      !tree["active"].null() || !tree["batch"].null() || !history.array() ||
      history.items().empty())
    return false;
  const auto &previous = history.items().back();
  const auto limit = previous.num("max_model_calls", 0);
  return previous.object() &&
         (previous.str("reason") == "initial_budget" ||
          previous.str("reason") == "budget_extension") &&
         limit > 0 && tree.num("model_calls", limit + 1) <= limit &&
         config.num("max_model_calls", 0) > limit;
}

// 调用方仍须完成 job/user/source 校验；此处仅移除可由扩容解除的派生标志。
inline bool clear_extended_gepa_reflection_exhaustion(Json &tree, const Json &config) {
  if (!gepa_model_budget_extended(tree, config)) return false;
  tree.fields().erase("reflection_exhausted");
  return true;
}

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
  Json memory_feedback;
  std::vector<GepaFinalist> finalists;
};

// 从唯一记忆库读取已获得训练支持的修订，作为下一次评测的种子。
// 只应用仍匹配来源修订和父组件的修改，并重新执行完整策略编译校验。
// 返回的种子仍需真实评测；此入口不会发布策略或调用远程模型。
Json apply_gepa_memory_feedback(const Json &artifact, const Json &feedback,
                                const Json &control_learning);

// 反证只作为反思的历史输入，不成为运行时规则或原经验的全局否定。
// 只选相同来源、背景及评分口径，去掉任务正文、报告路径、时间戳等非必要信息。
Json gepa_reflection_rejections(const Json &artifact, const Json &feedback,
                                const Json &control_learning,
                                const std::string &evaluation_context);

// 读取外部搜索参数并检查范围与能力依赖，配置变化不需要改写搜索代码。
Json load_gepa_config(const std::filesystem::path &path);
// 为影响搜索行为的配置生成稳定身份，供断点续跑校验。
std::string gepa_config_fingerprint(const Json &config);
// 驱动多轮候选生成、训练反馈、验证选择和冻结留出评测。
// 所有阶段受同一预算和取消信号约束，缓存必须匹配数据、宿主与评分版本。
GepaResult run_gepa_search(
    const Json &seed, const Json &baseline, const std::filesystem::path &dataset,
    const std::filesystem::path &evaluation_path, const Json &evaluation, const Json &config,
    const Json &gate, const Json &control_learning, const std::filesystem::path &output,
    const std::string &context, const std::string &evaluation_context,
    const std::string &candidate_evaluation_context, const std::function<bool()> &cancelled,
    std::shared_ptr<agentflow::trajectory::ModelAdapter> reflection_model = nullptr,
    std::size_t final_holdout_top_k_override = 0, bool final_holdout_replay = false,
    const Json &reflection_history = {},
    const std::string &final_holdout_execution_context = {});

} // 命名空间结束：trajectory::evolution::integration
