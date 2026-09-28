// 公共与私有经验管理：处理经验关系、跨用户支持、冲突、晋升与淘汰。
// 共享前检查来源与隐私，不能把带有用户专属条件的经验直接泛化给所有用户。
// 策略编译需同时考虑私有覆盖和公共可信度，不能把重复经验叠加为额外支持。

#pragma once

#include <functional>
#include "integration_json.h"
#include "trajectory_evolution/shadow_runtime.h"

namespace trajectory::evolution::integration {

using PrivateMemoryLoader =
    std::function<agentflow::trajectory::evaluation::EvolutionMemoryStore(const std::string &)>;
using SharedCheckpoint = std::function<void(const Json &)>;
using PublicEvidenceCheck = std::function<Json(
    const std::string &, const agentflow::trajectory::evaluation::EvolutionMemoryEntry &)>;

// 这些函数只在单个后台工作线程执行，不持久化原始模型文本。
// 检查点只包含已审查公共内容和不透明来源标识。
Json shared_memory_options(Json options);
// 初始化公共库状态和版本，不包含任何未审查的用户私有内容。
Json new_shared_memory(const std::string &application);
// 验证公共库内容、来源和关系状态，损坏条目不能参与策略编译。
void validate_shared_memory(const Json &state, const std::string &application, const Json &options);
// 协调公共经验与各用户私有经验的支持、分歧和版本关系。
// 私有冲突会降低公共条目的可信状态；不能把不同用户的支持简单相加后覆盖反证。
void reconcile_shared_memory(Json &state, const Json &options);
// 将同库 GEPA 证据投影到已有公共台账，不复制私有正文或组合策略。
// 私有来源失效只撤回该来源的支持；公共原文的独立反证使其进入 disputed 待复核，
// 不删除公共条目，也不凭一个用户的正向效果跳过共享审查或支持门槛。
void reconcile_shared_feedback(Json &state, const Json &config, const std::string &user,
    const agentflow::trajectory::evaluation::EvolutionMemoryStore &memory);

// 把待管理的用户经验变化加入公共库后台队列，避免热路径同步扫描全库。
void enqueue_shared_memory(Json &state, const std::string &user);
// 批量执行共享管理，模型预算与检查点贯穿处理中断、重试和恢复。
void process_shared_memory(Json &state, const Json &config, const PrivateMemoryLoader &load,
                           const std::shared_ptr<agentflow::trajectory::ModelAdapter> &model,
                           const SharedCheckpoint &checkpoint, const PublicEvidenceCheck &evidence);
v1::SharedExperienceStore shared_memory_view(const Json &state, const std::string &user = {});
// 计算公共库有效内容的版本，供策略编译判断是否需要更新。
std::string shared_content_revision(const v1::PublishedExperience &experience);

} // 命名空间结束：trajectory::evolution::integration
