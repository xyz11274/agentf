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

class AgentTask;

/**
 * AgentSession represents a session context for an agent execution
 * Manages session lifecycle, state, and metadata
 */
class AgentSession {
public:
  enum class SessionState {
    CREATED    = 0,
    ACTIVE     = 1,
    PAUSED     = 2,
    COMPLETED  = 3,
    TERMINATED = 4,
  };

  struct Config {
    std::string session_id;
    int         max_queue_size = 1024;
    int64_t     timeout_ms = 30000;  // 30 seconds default
    int         max_retries = 3;
  };

  explicit AgentSession(const Config& config);
  ~AgentSession();

  // Getters
  const std::string& get_session_id() const { return config_.session_id; }
  SessionState       get_state() const { return state_; }
  const Config&      get_config() const { return config_; }
  int64_t            get_created_time() const { return created_time_; }
  int64_t            get_last_activity_time() const { return last_activity_time_; }

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
  bool enqueue_received_message(const AgentMsgQue::Message& message, bool high_priority = false);
  bool dequeue_received_message(AgentMsgQue::Message& message);
  bool enqueue_send_message(const AgentMsgQue::Message& message);
  bool dequeue_send_message(AgentMsgQue::Message& message);
  int  get_receive_queue_size() const;
  int  get_send_queue_size() const;

  // Session task ownership
  void                                      add_task(const std::shared_ptr<AgentTask>& task);
  std::shared_ptr<AgentTask>                get_task(const std::string& task_id) const;
  std::vector<std::shared_ptr<AgentTask>>   get_tasks() const;

private:
  Config                                      config_;
  SessionState                                state_ = SessionState::CREATED;
  int64_t                                     created_time_ = 0;
  int64_t                                     last_activity_time_ = 0;
  std::map<std::string, std::string>          metadata_;
  AgentMsgQue                                 receive_queue_;
  AgentMsgQue                                 send_queue_;
  std::map<std::string, std::shared_ptr<AgentTask>> tasks_;
  mutable std::mutex                          mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_SESSION_H
