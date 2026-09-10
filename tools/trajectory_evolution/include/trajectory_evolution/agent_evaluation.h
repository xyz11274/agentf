#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include "trajectory_evolution/policy_runtime.h"

namespace trajectory::evolution {

// Frozen behavior is rebound within the same application and user only.
v1::RuntimePolicy bind_runtime_policy(const v1::RuntimePolicy& policy,
                                      const v1::Identity& identity);
bool published_policy_compatible(const v1::RuntimePolicy& published,
                                 const v1::RuntimePolicy& current,
                                 const std::string& control_learning_json = {});

// Shared L0-L4 evaluator. Submission and I/O belong to management/background
// callers, never to model/action/loop hot paths. Each trial runs a separate host.
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
  std::string agent_configuration() const;
  void retry(const std::string& user, const std::string& job_id);
  void cancel(const std::string& user, const std::string& job_id);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace trajectory::evolution
