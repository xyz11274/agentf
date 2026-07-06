#ifndef AGENTFLOW_INTERNAL_AGENT_TASK_H
#define AGENTFLOW_INTERNAL_AGENT_TASK_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/msgque.h"

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
    INTERRUPTED = 5,   // Task interrupted by external control
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
    int         workflow_pool_size = 1;
    bool        is_skill_instance = false;
    int         skill_instance_index = -1;
    bool        disable_context = false;
    int64_t     priority = 0;
    int         max_retries = 3;
    // Dispatcher and skill management fields
    std::vector<std::string> next_skills;           // Pre-configured skills to launch
    int         max_concurrent_skills = 10;          // Maximum concurrent skills for dispatcher
    // Skill parameter validation
    std::vector<std::string> expected_parameters;   // Required parameters for skill execution
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
  int                get_workflow_pool_size() const { return config_.workflow_pool_size; }
  bool               is_skill_instance() const { return config_.is_skill_instance; }
  int                get_skill_instance_index() const { return config_.skill_instance_index; }
  TaskState          get_state() const { return state_; }
  const Config&      get_config() const { return config_; }
  int64_t            get_created_time() const { return created_time_; }
  int64_t            get_completed_time() const { return completed_time_; }
  double             get_progress() const { return progress_; }
  const std::string& get_error_message() const { return error_message_; }
  // Dispatcher and skill management getters
  const std::vector<std::string>& get_next_skills() const { return config_.next_skills; }
  int                get_max_concurrent_skills() const { return config_.max_concurrent_skills; }
  // Skill parameter validation getters
  const std::vector<std::string>& get_expected_parameters() const { return config_.expected_parameters; }

  bool start();
  bool complete(const std::string& result = "");
  bool fail(const std::string& error_message);
  bool cancel();
  bool interrupt();  // Set state to INTERRUPTED and stop message threads
  bool retry();

  void set_progress(double progress);
  void increment_progress(double delta);

  void               set_parameter(const std::string& key, const std::string& value);
  bool               get_parameter(const std::string& key, std::string& value) const;
  void               set_result(const std::string& result);
  const std::string& get_result() const { return result_; }

  int get_retry_count() const { return retry_count_; }
  int increment_retry_count();

  using MessageHandler = std::function<void(const MMAgentMsgQue::Message&)>;

  void configure_message_handlers(MessageHandler intent_handler, MessageHandler data_handler);
  bool start_message_threads(MessageHandler intent_handler, MessageHandler data_handler);
  bool start_message_threads();  // 无参：复用已有 handler 重新拉起线程
  void stop_message_threads();
  bool are_message_threads_running() const { return message_threads_running_; }

  bool enqueue_intent_request_message(const MMAgentMsgQue::Message& message);
  bool enqueue_data_exchange_message(const MMAgentMsgQue::Message& message);
  bool enqueue_response_message(const MMAgentMsgQue::Message& message);
  bool dequeue_response_message(MMAgentMsgQue::Message& message);
  int  process_pending_messages_once();
  int  get_intent_request_queue_size() const;
  int  get_data_exchange_queue_size() const;
  int  get_response_queue_size() const;

  void register_dispatch_record(const MMAgentMsgQue::Message& message);
  void acknowledge_dispatch_record(const std::string& message_id);

  // Skill management methods (for dispatcher and coordinator tasks)
  int                get_running_skills_count() const;
  bool               can_launch_more_skills() const;
  void               on_skill_started(const std::string& skill_name);
  void               on_skill_completed(const std::string& skill_name);
  void               clear_running_skills();

 private:
  struct DispatchRecord {
    MMAgentMsgQue::Message message;
    int64_t                last_sent_time_ms = 0;
    bool                   awaiting_response = false;
    int                    resend_count = 0;
  };

  static int64_t now_ms();
  void           intent_request_loop();
  void           data_exchange_loop();
  void           monitor_loop();
  int            process_intent_request_queue_once();
  int            process_data_exchange_queue_once();

  Config                             config_;
  TaskState                          state_ = TaskState::PENDING;
  int64_t                            created_time_ = 0;
  int64_t                            completed_time_ = 0;
  double                             progress_ = 0.0;
  std::string                        error_message_;
  std::string                        result_;
  int                                retry_count_ = 0;
  std::map<std::string, std::string> parameters_;
  std::deque<MMAgentMsgQue::Message> intent_request_messages_;
  std::deque<MMAgentMsgQue::Message> data_exchange_messages_;
  std::deque<MMAgentMsgQue::Message> response_messages_;  // Responses from other tasks (e.g., skill -> coordinator)
  std::map<std::string, DispatchRecord> dispatch_records_;
  MessageHandler                     intent_request_handler_;
  MessageHandler                     data_exchange_handler_;
  std::atomic<bool>                  message_threads_running_{false};
  std::thread                        intent_request_thread_;
  std::thread                        data_exchange_thread_;
  std::thread                        monitor_thread_;
  std::condition_variable            queue_cv_;
  mutable std::mutex                 mutex_;
  // Skill management for dispatcher/coordinator tasks
  std::atomic<int>                   running_skills_count_{0};
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_TASK_H
