#ifndef AGENTFLOW_INTERNAL_AGENT_TASK_H
#define AGENTFLOW_INTERNAL_AGENT_TASK_H

#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <mutex>

namespace agentflow {
namespace framework {

class AgentContext;

/**
 * AgentTask represents a single task executed within an agent
 * Manages task execution state, progress, and results
 */
class AgentTask {
public:
  enum class TaskState {
    PENDING    = 0,
    RUNNING    = 1,
    COMPLETED  = 2,
    FAILED     = 3,
    CANCELLED  = 4,
  };

  struct Config {
    std::string task_id;
    std::string task_type;
    std::string description;
    std::string session_id;
    std::string workflow_id;
    std::string workflow_file;
    std::string task_context_workflow;
    std::string collaboration_so_path;
    int64_t     priority = 0;
    int         max_retries = 3;
  };

  explicit AgentTask(const Config& config);
  ~AgentTask();

  // Getters
  const std::string& get_task_id() const { return config_.task_id; }
  const std::string& get_task_type() const { return config_.task_type; }
  const std::string& get_session_id() const { return config_.session_id; }
  const std::string& get_workflow_id() const { return config_.workflow_id; }
  const std::string& get_workflow_file() const { return config_.workflow_file; }
  const std::string& get_collaboration_so_path() const { return config_.collaboration_so_path; }
  std::shared_ptr<AgentContext> get_context() const { return context_; }
  TaskState          get_state() const { return state_; }
  const Config&      get_config() const { return config_; }
  int64_t            get_created_time() const { return created_time_; }
  int64_t            get_completed_time() const { return completed_time_; }
  double             get_progress() const { return progress_; }
  const std::string& get_error_message() const { return error_message_; }

  // Task lifecycle
  bool start();
  bool complete(const std::string& result = "");
  bool fail(const std::string& error_message);
  bool cancel();
  bool retry();

  // Progress tracking
  void set_progress(double progress);
  void increment_progress(double delta);

  // Task parameters and results
  void       set_parameter(const std::string& key, const std::string& value);
  bool       get_parameter(const std::string& key, std::string& value) const;
  void       set_result(const std::string& result);
  const std::string& get_result() const { return result_; }

  // Retry tracking
  int get_retry_count() const { return retry_count_; }
  int increment_retry_count();

private:
  Config                                      config_;
  TaskState                                   state_ = TaskState::PENDING;
  int64_t                                     created_time_ = 0;
  int64_t                                     completed_time_ = 0;
  double                                      progress_ = 0.0;
  std::string                                 error_message_;
  std::string                                 result_;
  int                                         retry_count_ = 0;
  std::map<std::string, std::string>          parameters_;
  std::shared_ptr<AgentContext>               context_;
  mutable std::mutex                          mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_TASK_H
