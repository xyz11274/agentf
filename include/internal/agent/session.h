#ifndef AGENTFLOW_INTERNAL_AGENT_SESSION_H
#define AGENTFLOW_INTERNAL_AGENT_SESSION_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/msgque.h"

namespace agentflow {
namespace framework {

class MMAgentTask;
class SessionTaskCoordinator;

/// Generate a UUID format string for session IDs.
/// Returns a string in format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
std::string generate_session_uuid();

class MMAgentSession {
 public:
  enum class SessionState {
    CREATED = 0,
    ACTIVE = 1,
    PAUSED = 2,
    COMPLETED = 3,
    TERMINATED = 4,
  };

  struct Config {
    int         session_index = -1;
    std::string session_id;
    std::string description;
    int         max_queue_size = 1024;
    int64_t     timeout_ms = 30000;
    int         max_retries = 3;
    std::string dispatcher_workflow;
    std::string coordinator_workflow;
    std::map<std::string, std::string> task_workflows;
    std::string task_context_workflow;
  };

  explicit MMAgentSession(const Config& config);
  ~MMAgentSession();

  const std::string& get_session_id() const { return config_.session_id; }
  int                get_session_index() const { return config_.session_index; }
  const std::string& get_description() const { return config_.description; }
  SessionState       get_state() const { return state_; }
  const Config&      get_config() const { return config_; }
  int64_t            get_created_time() const { return created_time_; }
  int64_t            get_last_activity_time() const { return last_activity_time_; }
  const std::string& get_active_route_key() const { return active_route_key_; }
  const std::string& get_user_id() const { return active_user_id_; }
  const std::string& get_client_id() const { return active_client_id_; }
  bool               is_assigned() const { return !active_route_key_.empty(); }
  bool               is_available() const { return active_route_key_.empty(); }
  std::shared_ptr<SessionTaskCoordinator> get_task_coordinator() const { return task_coordinator_; }
  std::shared_ptr<SessionTaskCoordinator> get_task_dispatcher() const { return task_dispatcher_; }

  bool activate();
  bool pause();
  bool resume();
  bool complete();
  bool terminate();

  void bind_route(const std::string& user_id, const std::string& client_id);
  void reset_for_pool();

  void set_metadata(const std::string& key, const std::string& value);
  bool get_metadata(const std::string& key, std::string& value) const;
  bool has_metadata(const std::string& key) const;
  void clear_metadata();

  void update_activity();
  bool is_timeout_expired() const;

  bool enqueue_received_message(const MMAgentMsgQue::Message& message);
  bool dequeue_received_message(MMAgentMsgQue::Message& message);
  bool dequeue_intent_request_message(MMAgentMsgQue::Message& message);
  bool dequeue_data_exchange_message(MMAgentMsgQue::Message& message);
  bool enqueue_send_message(const MMAgentMsgQue::Message& message);
  bool dequeue_send_message(MMAgentMsgQue::Message& message);
  int  get_receive_queue_size() const;
  int  get_send_queue_size() const;

  void add_template_task(const std::shared_ptr<MMAgentTask>& task);
  void add_task(const std::shared_ptr<MMAgentTask>& task);
  std::shared_ptr<MMAgentTask>              get_task(const std::string& task_id) const;
  std::vector<std::shared_ptr<MMAgentTask>> get_tasks() const;
  int                                       get_template_task_count() const;
  void set_task_coordinator(const std::shared_ptr<SessionTaskCoordinator>& coordinator);
  void set_task_dispatcher(const std::shared_ptr<SessionTaskCoordinator>& dispatcher);
  void set_dispatcher_task(const std::shared_ptr<MMAgentTask>& task);
  void set_coordinator_task(const std::shared_ptr<MMAgentTask>& task);
  std::shared_ptr<MMAgentTask> get_dispatcher_task() const;
  std::shared_ptr<MMAgentTask> get_coordinator_task() const;

  // Clear running skills count on dispatcher/coordinator tasks (for session pool reuse)
  void clear_running_skills();

  // Clear all runtime tasks (for session destroy)
  void clear_all_tasks();

 private:
  Config      config_;
  SessionState state_ = SessionState::CREATED;
  int64_t     created_time_ = 0;
  int64_t     last_activity_time_ = 0;
  std::string active_route_key_;
  std::string active_user_id_;
  std::string active_client_id_;
  std::map<std::string, std::string> metadata_;
  std::map<std::string, std::shared_ptr<MMAgentTask>> template_tasks_;
  std::map<std::string, std::shared_ptr<MMAgentTask>> runtime_tasks_;
  std::shared_ptr<SessionTaskCoordinator> task_dispatcher_;
  std::shared_ptr<SessionTaskCoordinator> task_coordinator_;
  std::shared_ptr<MMAgentTask> dispatcher_task_;
  std::shared_ptr<MMAgentTask> coordinator_task_;
  mutable std::mutex mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_SESSION_H
