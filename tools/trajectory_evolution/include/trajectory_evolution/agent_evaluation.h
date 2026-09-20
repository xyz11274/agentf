#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include "trajectory_evolution/policy_runtime.h"

namespace trajectory::evolution {

// 冻结行为只能在同一应用与同一用户范围内重新绑定。
v1::RuntimePolicy bind_runtime_policy(const v1::RuntimePolicy& policy,
                                      const v1::Identity& identity);
bool published_policy_compatible(const v1::RuntimePolicy& published,
                                 const v1::RuntimePolicy& current,
                                 const std::string& control_learning_json = {});

// L0-L4 共用评测器。提交和 I/O 属于管理／后台调用方，绝不能进入模型、动作、循环的
// 热路径；每次试验都在独立宿主中运行。
class AgentEvaluationService {
 public:
  AgentEvaluationService(const std::filesystem::path& config,
                         const std::filesystem::path& data_directory,
                         std::function<void()> changed = {},
                         std::string control_learning_json = {});
  ~AgentEvaluationService();
  AgentEvaluationService(const AgentEvaluationService&) = delete;
  AgentEvaluationService& operator=(const AgentEvaluationService&) = delete;
  std::string submit(const std::string& user, const std::string& artifact_json);
  std::string status(const std::string& user) const;
  std::string publications(const std::string& user) const;
  std::string feedback(const std::string& user) const;
  std::string search_tree(const std::string& user, const std::string& job_id) const;
  std::string replay_holdout(const std::string& user, const std::string& job_id);
  std::string agent_configuration() const;
  // 只在后台准备种子。训练与评测共用隔离宿主、队列和限制，
  // 但绝不发布策略或分数。
  std::string prepare_training(const std::string& user, const std::string& seed_json,
                               bool learning_pending, bool learning_failed);
  void complete_training_import(const std::string& user, const std::string& job_id,
                                const std::string& result_json);
  void retry(const std::string& user, const std::string& job_id);
  void cancel(const std::string& user, const std::string& job_id);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace trajectory::evolution
