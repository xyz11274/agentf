#ifndef AGENTFLOW_INTERNAL_AGENT_SESSION_H
#define AGENTFLOW_INTERNAL_AGENT_SESSION_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
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

/// Session event type for lifecycle callbacks
enum class SessionEventType {
  CREATED,    // Session bound to user/client (bind_route called)
  DESTROYED,  // Session returned to pool (reset_for_pool called)
};

/// Session event callback signature
/// Parameters: session_id, user_id, client_id, event_type
using SessionEventCallback = std::function<void(
    const std::string& session_id,
    const std::string& user_id,
    const std::string& client_id,
    SessionEventType event_type)>;

/// Dialogue event type for lifecycle callbacks
enum class DialogueEventType {
  CREATED,    // Dialogue created (dialogue_id generated)
  DESTROYED,  // Dialogue cleaned up
};

/// Dialogue event callback signature
/// Parameters: agent_id, session_id, dialogue_id, user_input, event_type
using DialogueEventCallback = std::function<void(
    const std::string& agent_id,
    const std::string& session_id,
    const std::string& dialogue_id,
    const std::string& user_input,
    DialogueEventType event_type)>;

/// Task event type for lifecycle callbacks
enum class TaskEventType {
  CREATED,    // Task added to session
  DESTROYED,  // Task completed/removed
};

/// Task event callback signature
/// Parameters: agent_id, session_id, dialogue_id, task_id, workflow_id, task_type, skill_name, event_type
using TaskEventCallback = std::function<void(
    const std::string& agent_id,
    const std::string& session_id,
    const std::string& dialogue_id,
    const std::string& task_id,
    const std::string& workflow_id,
    const std::string& task_type,
    const std::string& skill_name,
    TaskEventType event_type)>;

/// Forward declaration for RAII guard (defined after MMAgentSession).
class MMAgentSession;

/// RAII guard for session-level dialogue serialization lock.
///
/// Returned by MMAgentSession::acquire_dialogue_lock.  Holding this guard
/// means the calling dialogue owns the session's single execution slot;
/// destroying it releases the slot and wakes the next queued dialogue.
///
/// Invariants:
/// - Destroying the guard exactly once releases the lock (RAII).
/// - is_cancelled() == true means the session was destroyed while waiting;
///   the caller must NOT proceed with dialogue execution.
class DialogueLockGuard {
 public:
  DialogueLockGuard(MMAgentSession& session, std::string dialogue_id);
  ~DialogueLockGuard();
  DialogueLockGuard(const DialogueLockGuard&)            = delete;
  DialogueLockGuard& operator=(const DialogueLockGuard&) = delete;
  DialogueLockGuard(DialogueLockGuard&&)                 = delete;
  DialogueLockGuard& operator=(DialogueLockGuard&&)      = delete;

  bool is_cancelled() const { return cancelled_; }
  const std::string& dialogue_id() const { return dialogue_id_; }

 private:
  MMAgentSession& session_;
  std::string     dialogue_id_;
  bool            cancelled_ = false;
};

class MMAgentSession {
 public:
  friend class DialogueLockGuard;
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
    int64_t     timeout_ms = 300000;
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

  // ── Dialogue serialization lock ──────────────────────────────────
  // Acquire the session's single dialogue execution slot.  Blocks until
  // the previous dialogue releases the lock OR the session is cancelled.
  // Returns nullptr if the session was cancelled while waiting (caller
  // must NOT proceed).  The returned guard is transferred to
  // hold_dialogue_lock() — the caller does NOT keep it on the stack
  // because dialogue workflow execution outlives handle_intent_request().
  std::unique_ptr<DialogueLockGuard> acquire_dialogue_lock(const std::string& dialogue_id);

  // Take ownership of a lock guard for the given dialogue_id.  The lock
  // will be released when release_dialogue_lock(dialogue_id) is called
  // (from cleanup_dialogue_data) OR when cancel_all_dialogues() clears
  // the entire map.  Pass nullptr to abort a dialogue before it starts.
  void hold_dialogue_lock(const std::string& dialogue_id,
                           std::unique_ptr<DialogueLockGuard> guard);

  // Release the lock held for dialogue_id, if any.  Idempotent.
  // Called from cleanup_dialogue_data (the single dialogue-completion
  // convergence point).
  void release_dialogue_lock(const std::string& dialogue_id);

  // Cancel the dialogue currently holding the lock (if any) and mark the
  // session as cancelled so any blocked acquire_dialogue_lock waiters
  // wake and return nullptr.  Called by handle_session_destroy /
  // handle_session_interrupt.  Idempotent.
  void cancel_all_dialogues();

  // Returns the dialogue_id currently holding the serialization lock, or
  // empty string if the slot is free.  Test/diagnostic helper.
  std::string get_active_dialogue_id() const;

  // Session event callback for lifecycle notifications
  void set_session_event_callback(SessionEventCallback callback) {
    session_event_callback_ = std::move(callback);
  }
  void clear_session_event_callback() {
    session_event_callback_ = nullptr;
  }

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
  SessionEventCallback          session_event_callback_;

  // ── Dialogue serialization lock state ────────────────────────────
  // At most one dialogue may execute in this session at a time.  Waiters
  // block on dialogue_serialize_cv_ until the holder releases or the
  // session is cancelled (session_cancelled_ flips to true).
  mutable std::mutex      dialogue_serialize_mutex_;
  std::condition_variable dialogue_serialize_cv_;
  std::string             active_dialogue_id_;   // empty = slot free
  std::atomic<bool>       session_cancelled_{false};

  // Locks held across async workflow execution.  Keyed by dialogue_id.
  // The guard's destructor releases the serialize slot when erase()d.
  std::map<std::string, std::unique_ptr<DialogueLockGuard>> held_locks_;

  // Release the slot held by the given dialogue_id.  Called by
  // ~DialogueLockGuard.  No-op if the slot is already free or held by a
  // different dialogue (defensive against double-release).
  void release_dialogue_slot(const std::string& dialogue_id);

  mutable std::mutex mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_SESSION_H
