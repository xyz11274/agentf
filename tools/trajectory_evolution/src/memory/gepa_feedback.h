// GEPA 训练反馈：把真实父子配对转换为同一记忆库中的有界策略修订。
// 不读取验证/留出任务，不调用模型，也不把组合收益分摊成每条经验的收益。
#pragma once

#include "integration_json.h"
#include "hybrid_evaluation.pb.h"
#include "shadow_policy.pb.h"
#include "trajectory_evolution/task_evaluation.h"

namespace trajectory::evolution::integration {

// 只记录已执行改动涉及的组件；返回空对象表示本次没有可归因的完整配对。
Json make_gepa_memory_feedback(
    const Json &before, const Json &after, const v1::ShadowPolicySnapshot &snapshot,
    const dataset::v1::TaskEvaluationReport &parent,
    const dataset::v1::TaskEvaluationReport &candidate,
    const Json &parent_scores, const Json &candidate_scores, const Json &applied,
    const std::string &dataset_version, const std::string &comparison_context,
    bool screening_passed);

// 来源内容或批准状态变化后旧修订不再用于编译；历史反馈仍可保留供审计。
bool gepa_feedback_sources_match(const Json &record, const v1::ShadowPolicySnapshot &snapshot);

// 未改动的背景也是组合的一部分，只存指纹，不复制完整策略。
bool gepa_feedback_context_matches(const Json &record, const Json &components);

// 合并到权威记忆的 gepa_feedback 字段；同题重复重放不增加支持题数。
// 调用方负责使用训练入库同一文件锁完成 Protobuf/JSON 提交。
Json merge_gepa_memory_feedback(const Json &current, const Json &incoming,
                                const v1::ShadowPolicySnapshot &snapshot,
                                std::size_t minimum_pairs = 2);

// 仅原文的单变量启停可归因到原经验；改写和控制组合的收益留在对应修订记录。
// 合并既有普通评测反馈时取支持计数的上界而不相加，避免同题被两个入口重复计票。
agentflow::trajectory::evaluation::MemoryEvaluationFeedback gepa_source_feedback(
    const agentflow::trajectory::evaluation::EvolutionMemoryStore &memory,
    const v1::ShadowExperience &source,
    const agentflow::trajectory::evaluation::MemoryEvaluationFeedback &ordinary = {});

// 在同一个记忆文件事务内更新私有经验状态；公共反馈交给共享管理器，
// 不在私有生命周期函数中直接修改公共条目或分摊组合收益。
void update_gepa_memory_states(agentflow::trajectory::evaluation::EvolutionMemoryStore &memory,
                               const v1::ShadowPolicySnapshot &snapshot);

} // 命名空间结束：trajectory::evolution::integration
