#ifndef AGENTFLOW_INTERNAL_AGENT_SESSION_H
#define AGENTFLOW_INTERNAL_AGENT_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <vector>

#include "agent/msgque.h"

namespace agentflow {
namespace framework {

class MMAgentTask;
class AgentContext;
class SessionTaskCoordinator;

/**
 * MMAgentSession represents a session context for an agent execution
 * Manages session lifecycle, state, and metadata
 */
class MMAgentSession {
public:
  enum class SessionState {
    CREATED    = 0,
    ACTIVE     = 1,
    PAUSED     = 2,
    COMPLETED  = 3,
    TERMINATED = 4,
  };

  struct Config {
    int         session_index = -1;
    std::string session_id;
    std::string session_type = "talker";
    std::string description;
    int         max_queue_size = 1024;
    int64_t     timeout_ms = 30000;  // 30 seconds default
    int         max_retries = 3;
    std::string dispatcher_workflow;
    std::string coordinator_workflow;
    std::map<std::string, std::string> task_workflows;
    std::string session_context_workflow;
    std::string task_context_workflow;
  };

  explicit MMAgentSession(const Config& config);
  ~MMAgentSession();

  // Getters
  const std::string& get_session_id() const { return config_.session_id; }
  int                get_session_index() const { return config_.session_index; }
  const std::string& get_session_type() const { return config_.session_type; }
  const std::string& get_description() const { return config_.description; }
  SessionState       get_state() const { return state_; }
  const Config&      get_config() const { return config_; }
  int64_t            get_created_time() const { return created_time_; }
  int64_t            get_last_activity_time() const { return last_activity_time_; }
  std::shared_ptr<AgentContext> get_context() const { return context_; }
  std::shared_ptr<SessionTaskCoordinator> get_task_coordinator() const { return task_coordinator_; }
  std::shared_ptr<SessionTaskCoordinator> get_task_dispatcher() const { return task_dispatcher_; }

  // Session lifecycle
  bool activate();
  bool pause();
  bool resume();
  bool complete();
  bool terminate();

  // Session metadata
  void       set_metadata(const std::string& key, const std::string& value);
  bool       get_metadata(const std::string& key, std::string& value) const;
  bool       has_metadata(const std::string& key) const;
  void       clear_metadata();

  // Activity tracking
  void update_activity();
  bool is_timeout_expired() const;

  // Queue operations
  bool enqueue_received_message(const MMAgentMsgQue::Message& message);
  bool dequeue_received_message(MMAgentMsgQue::Message& message);
  bool dequeue_intent_request_message(MMAgentMsgQue::Message& message);
  bool dequeue_output_result_message(MMAgentMsgQue::Message& message);
  bool enqueue_send_message(const MMAgentMsgQue::Message& message);
  bool dequeue_send_message(MMAgentMsgQue::Message& message);
  int  get_receive_queue_size() const;
  int  get_send_queue_size() const;

  // Session task ownership
  void                                      add_task(const std::shared_ptr<MMAgentTask>& task);
  std::shared_ptr<MMAgentTask>                get_task(const std::string& task_id) const;
  std::vector<std::shared_ptr<MMAgentTask>>   get_tasks() const;
  void                                      set_task_coordinator(
      const std::shared_ptr<SessionTaskCoordinator>& coordinator);
  void                                      set_task_dispatcher(
      const std::shared_ptr<SessionTaskCoordinator>& dispatcher);

private:
  Config                                      config_;
  SessionState                                state_ = SessionState::CREATED;
  int64_t                                     created_time_ = 0;
  int64_t                                     last_activity_time_ = 0;
  std::map<std::string, std::string>          metadata_;
  MMAgentMsgQue                                 receive_queue_;
  MMAgentMsgQue                                 send_queue_;
  std::map<std::string, std::shared_ptr<MMAgentTask>> tasks_;
  std::shared_ptr<SessionTaskCoordinator>     task_dispatcher_;
  std::shared_ptr<SessionTaskCoordinator>     task_coordinator_;
  std::shared_ptr<AgentContext>               context_;
  mutable std::mutex                          mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_SESSION_H
