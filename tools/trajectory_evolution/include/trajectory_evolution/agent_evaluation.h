// 后台评测服务：管理候选作业、训练引导、GEPA 调度与发布验收。
// 主 Agent 的打点不等待这里的模型评测；队列、取消和频率限制由服务统一维护。
// 搜索分数与发布结论分开：候选表现改善不等于已经满足发布条件。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include "trajectory_evolution/policy_runtime.h"

namespace trajectory::evolution {

// 冻结行为只能在同一应用与同一用户范围内重新绑定。
v1::RuntimePolicy bind_runtime_policy(const v1::RuntimePolicy &policy,
                                      const v1::Identity &identity);
bool published_policy_compatible(const v1::RuntimePolicy &published,
                                 const v1::RuntimePolicy &current,
                                 const std::string &control_learning_json = {});

// L0-L4 共用评测器。提交和 I/O 属于管理／后台调用方，绝不能进入模型、动作、循环的
// 热路径；每次试验都在独立宿主中运行。
class AgentEvaluationService {
public:
  AgentEvaluationService(const std::filesystem::path &config,
                         const std::filesystem::path &data_directory,
                         std::function<void()> changed = {},
                         std::string control_learning_json = {});
  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~AgentEvaluationService();
  AgentEvaluationService(const AgentEvaluationService &) = delete;
  AgentEvaluationService &operator=(const AgentEvaluationService &) = delete;
  // 校验并提交后台评测作业，返回可查询身份，不在主 Agent 热路径等待整场实验。
  std::string submit(const std::string &user, const std::string &artifact_json);
  // 后台管理器可同时交付原库反馈；仅提取有界反证，不改变已冻结的运行策略。
  // 保留双参数入口，原有 C++ 调用方及各语言打点接口无需修改。
  std::string submit(const std::string &user, const std::string &artifact_json,
                     const std::string &memory_feedback_json);
  // 返回当前运行或作业状态，不触发额外执行或重新评测。
  std::string status(const std::string &user) const;
  // 返回已发布策略的可用版本与元信息，不把搜索中的候选当作正式版本。
  std::string publications(const std::string &user) const;
  // 接收并关联评测反馈，维持候选、运行与来源经验之间的对应关系。
  std::string feedback(const std::string &user) const;
  // 交付已完成配对的训练反馈；停止检查点同样可恢复，但不发布中间策略。
  // 先按当前评分口径校验，再写权威记忆并确认；崩溃重放保持幂等。
  std::string gepa_feedback(const std::string &user) const;
  void complete_gepa_feedback(const std::string &user, const std::string &job_id,
                              const std::string &result_json);

  // 读取本次搜索的节点、父子与融合关系，供展示和复盘，不修改候选策略。
  std::string search_tree(const std::string &user, const std::string &job_id) const;
  // 在冻结候选上执行留出评测，留出结果不反馈到当轮策略改写。
  std::string replay_holdout(const std::string &user, const std::string &job_id);
  // 返回当前宿主配置视图，供后台创建一致的评测环境。
  std::string agent_configuration() const;
  // 只在后台准备种子。训练与评测共用隔离宿主、队列和限制，
  // 但绝不发布策略或分数。
  std::string prepare_training(const std::string &user, const std::string &seed_json,
                               bool learning_pending, bool learning_failed);
  // 确认训练轨迹完成导入并更新后台状态，避免同批次重复积累经验。
  void complete_training_import(const std::string &user, const std::string &job_id,
                                const std::string &result_json);
  // 为允许重试的失败作业恢复执行入口，保留固定输入与幂等身份。
  void retry(const std::string &user, const std::string &job_id);
  // 请求取消当前作业，保留已落盘结果；在途执行由监督逻辑安全收尾。
  void cancel(const std::string &user, const std::string &job_id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // 命名空间结束：trajectory::evolution
