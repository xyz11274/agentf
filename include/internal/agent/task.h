#ifndef AGENTFLOW_INTERNAL_AGENT_TASK_H
#define AGENTFLOW_INTERNAL_AGENT_TASK_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentflow {
namespace framework {

class MMAgentTask {
 public:
  enum class TaskState {
    PENDING = 0,
    RUNNING = 1,
    COMPLETED = 2,
    FAILED = 3,
    CANCELLED = 4,
  };

  struct Config {
    int         task_index = -1;
    std::string task_id;
    std::string task_type;
    std::string description;
    std::string session_id;
    std::string workflow_id;
    std::string workflow_file;
    std::string context_workflow;
    std::string task_context_workflow;
    std::string collaboration_so_path;
    std::string skill_name;
    std::string skill_config_dir;
    int         instance_pool_size = 1;
    bool        is_skill_instance = false;
    int         skill_instance_index = -1;
    bool        disable_context = false;
    int64_t     priority = 0;
    int         max_retries = 3;
  };

  explicit MMAgentTask(const Config& config);
  ~MMAgentTask();

  const std::string& get_task_id() const { return config_.task_id; }
  const std::string& get_task_type() const { return config_.task_type; }
  int                get_task_index() const { return config_.task_index; }
  const std::string& get_session_id() const { return config_.session_id; }
  const std::string& get_workflow_id() const { return config_.workflow_id; }
  const std::string& get_workflow_file() const { return config_.workflow_file; }
  const std::string& get_context_workflow() const { return config_.context_workflow; }
  const std::string& get_collaboration_so_path() const { return config_.collaboration_so_path; }
  const std::string& get_skill_name() const { return config_.skill_name; }
  const std::string& get_skill_config_dir() const { return config_.skill_config_dir; }
  int                get_instance_pool_size() const { return config_.instance_pool_size; }
  bool               is_skill_instance() const { return config_.is_skill_instance; }
  int                get_skill_instance_index() const { return config_.skill_instance_index; }
  TaskState          get_state() const { return state_; }
  const Config&      get_config() const { return config_; }
  int64_t            get_created_time() const { return created_time_; }
  int64_t            get_completed_time() const { return completed_time_; }
  double             get_progress() const { return progress_; }
  const std::string& get_error_message() const { return error_message_; }

  bool start();
  bool complete(const std::string& result = "");
  bool fail(const std::string& error_message);
  bool cancel();
  bool retry();

  void set_progress(double progress);
  void increment_progress(double delta);

  void               set_parameter(const std::string& key, const std::string& value);
  bool               get_parameter(const std::string& key, std::string& value) const;
  void               set_result(const std::string& result);
  const std::string& get_result() const { return result_; }

  int get_retry_count() const { return retry_count_; }
  int increment_retry_count();

 private:
  Config                             config_;
  TaskState                          state_ = TaskState::PENDING;
  int64_t                            created_time_ = 0;
  int64_t                            completed_time_ = 0;
  double                             progress_ = 0.0;
  std::string                        error_message_;
  std::string                        result_;
  int                                retry_count_ = 0;
  std::map<std::string, std::string> parameters_;
  mutable std::mutex                 mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_TASK_H
