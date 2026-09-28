// 影子策略：从记忆快照编译候选经验，并执行检索与建议生成。
// 影子建议和已批准的运行控制分开，避免把候选经验直接当作发布策略。
// 快照绑定经验内容版本，公共/私有关系变化后需要重新核对兼容性。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <filesystem>
#include <map>
#include <set>

#include "shadow_policy.pb.h"
#include "hybrid_evaluation.pb.h"

namespace trajectory::evolution {

// 调用方绑定可信 Agent、用户和运行身份，并显式授权作用域。
// 只编译单用户且无冲突的经验候选。
v1::ShadowPolicySnapshot
compile_shadow_policy(const agentflow::trajectory::evaluation::EvolutionMemoryStore &memory,
                      const v1::Identity &identity, const v1::ShadowOptions &options);
// 应用公共发布使用独立且显式可信的输入。
// 私有所有权检查不变，记忆条目不能自行声明允许共享。
v1::ShadowPolicySnapshot
compile_shadow_policy(const agentflow::trajectory::evaluation::EvolutionMemoryStore &memory,
                      const v1::Identity &identity, const v1::ShadowOptions &options,
                      const v1::SharedExperienceStore &shared);
// 读取经验与配置后生成可持久化策略快照，加载时不依赖原实验目录。
v1::ShadowPolicySnapshot compile_shadow_policy_file(const std::filesystem::path &memory_path,
                                                    const v1::Identity &identity,
                                                    const v1::ShadowOptions &options);

// 不可变、不执行 I/O 或模型调用，也无共享可变缓存；并发查询安全。
v1::ShadowPolicySnapshot bind_shadow_policy(v1::ShadowPolicySnapshot snapshot,
                                            const v1::Identity &identity);

class ShadowRuntime {
public:
  // 初始化 ShadowRuntime 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit ShadowRuntime(v1::ShadowPolicySnapshot snapshot);

  // 返回已冻结策略快照，后续查询只读使用。
  const v1::ShadowPolicySnapshot &snapshot() const {
    return snapshot_;
  }

  // 在授权作用域内检索适用经验，遵守去重、冲突和上下文预算。
  v1::AdviceResult query_advice(const v1::AdviceQuery &query) const;
  // 在授权作用域内检索适用经验，遵守去重、冲突和上下文预算。
  v1::AdviceResult query_advice(const v1::AdviceQuery &query,
                                const std::set<std::string> &approved_ids) const;
  // 在授权作用域内检索适用经验，遵守去重、冲突和上下文预算。
  v1::AdviceResult query_advice(const v1::AdviceQuery &query,
                                const std::set<std::string> &approved_ids,
                                const std::map<std::string, std::string> &triggers) const;

private:
  // 基于当前观测和冻结策略查询建议或控制决定，查询成功不等于真实执行。
  v1::AdviceResult query(const v1::AdviceQuery &query, const std::set<std::string> *approved_ids,
                         const std::map<std::string, std::string> *triggers = nullptr) const;
  const v1::ShadowPolicySnapshot snapshot_;
};

// 这是自定义观测，不是 advice.applied，不能据此确认 L1 已生效。
v1::TraceEvent shadow_trace_event(const v1::AdviceResult &result, const std::string &producer_id,
                                  std::uint64_t sequence);

} // 命名空间结束：trajectory::evolution
