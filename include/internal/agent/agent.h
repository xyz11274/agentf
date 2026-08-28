#ifndef AGENTFLOW_FRAMEWORK_AGENT_H
#define AGENTFLOW_FRAMEWORK_AGENT_H

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agent/common.h"
#include "agent/msgque.h"
#include "agent/package.h"
#include "agent/session.h"
#include "workflow/workflow.h"
#include "utils/file_storage.h"
#include <memory>

namespace agentflow {
namespace framework {

class MMAgentSession;
class MMAgentTask;
class AgentContext;
class AgentPackage;
class SessionTaskCoordinator;
class SessionRouter;
}  // namespace framework

namespace runtime {
class MMDataBus;  // Forward declaration
}  // namespace runtime

// Forward declare test fixture for private member access (handle_dialogue_interrupt).
namespace tests { class DialogueInterruptFlagFixture; }

namespace framework {
class BuiltinDispatcher;

/// Context callback function type.
/// Called when a new dialogue is created to inject user context.
/// @param user_id The user identifier
/// @param agent_id The agent identifier
/// @param user_input The user's input text
/// @param previous_context The previous context string (may be empty for new dialogues)
/// @return The new context string to be stored in DataBus
using ContextCallback = std::function<std::string(
    const std::string& user_id,
    const std::string& agent_id,
    const std::string& user_input,
    const std::string& previous_context)>;

/// Configuration for context callback behavior
struct ContextCallbackConfig {
  ContextCallback callback;           ///< The callback function
  bool async = true;                  ///< Whether to call asynchronously (default: true)
  int timeout_ms = 5000;              ///< Timeout for synchronous calls (ms)
};

struct AgentMetadata {
  AgentId                 id;
  std::string             name;
  std::string             version;
  std::string             description;
  int64_t                 created_at;
  int64_t                 updated_at;
  std::string             author;
  std::vector<WorkflowId> workflow_ids;
};

class MMAgent {
 public:
  enum class LifecyclePolicy {
    PER_REQUEST = 0,
    FACTORY = 1,
    SINGLETON = 2,
  };

  struct Config {
    AgentId     id;
    std::string name;
    std::string version = "1.0";
    std::string description;
    LifecyclePolicy policy = LifecyclePolicy::PER_REQUEST;
    int max_workflows = 10;
    int max_concurrent_workflows = 5;
    int data_bus_capacity_mb = 1;  ///< DataBus capacity in megabytes (default: 1MB)
  };

  explicit MMAgent(const Config& config);
  ~MMAgent();

  const AgentId&     get_id() const { return config_.id; }
  const std::string& get_name() const { return config_.name; }
  AgentState         get_state() const { return state_; }
  const Config&      get_config() const { return config_; }

  std::shared_ptr<MMAgentSession> get_session(const std::string& session_key) const;
  std::vector<std::shared_ptr<MMAgentSession>> get_sessions() const;
  std::shared_ptr<MMAgentTask>    get_current_task() const { return current_task_; }
  std::shared_ptr<AgentContext>   get_context() const { return context_; }
  std::shared_ptr<AgentPackage>   get_package() const { return package_; }
  std::shared_ptr<SessionRouter>  get_session_router() const { return session_router_; }

  std::shared_ptr<MMAgentSession> get_or_create_session(const std::string& user_id,
                                                        const std::string& client_id);
  std::shared_ptr<MMAgentSession> get_session_by_key(const std::string& session_key) const;
  bool route_request_message(const MMAgentMsgQue::Message& message);
  bool dequeue_response_message(MMAgentMsgQue::Message& message);
  bool enqueue_response_message(const MMAgentMsgQue::Message& message);
  int  get_response_queue_size() const;
  int  process_session_messages(const std::string& session_key);
  int  process_all_sessions_until_idle();
  bool pop_generated_task(std::shared_ptr<MMAgentTask>& task);

  void add_workflow(std::shared_ptr<MMWorkflow> workflow);
  void remove_workflow(const WorkflowId& workflow_id);
  std::shared_ptr<MMWorkflow> get_workflow(const WorkflowId& workflow_id);
  const std::vector<std::shared_ptr<MMWorkflow>>& get_workflows() const { return workflows_; }

  bool initialize_resources();
  void cleanup_resources();

  bool publish_data(const std::string& key, const DataVariant& value);
  bool get_data(const std::string& key, DataVariant& value);

  // Access to Runtime's MMDataBus for unified data storage with nodes
  void set_runtime_databus(runtime::MMDataBus* databus) { runtime_databus_ = databus; }
  runtime::MMDataBus* get_runtime_databus() const { return runtime_databus_; }

  // DataBus cleanup functions
  /// Clear all DataBus data for a session.
  /// Uses remove_by_prefix to batch delete all keys under /session/{session_id}/
  /// @param session_id The session ID to clear
  /// @return Number of keys removed
  int clear_session_data(const std::string& session_id);

  /// Clear DataBus data for a session, but only if the session has NOT been
  /// reused since the caller captured its active_seq. This prevents the
  /// aging-callback reuse race (P0-1): when a pooled session is handed back to
  /// a new user between aging-timeout and the deferred cleanup callback, the
  /// stale callback would otherwise delete the new user's data. The caller
  /// passes the seq it observed at aging time; if the session's current seq
  /// differs (meaning activate() was called again → reused), cleanup is skipped.
  /// @param session_id The session ID to clear
  /// @param expected_seq The active_seq the caller observed at aging time
  /// @return Number of keys removed, or 0 if skipped due to reuse
  int clear_session_data_if_unused(const std::string& session_id, uint64_t expected_seq);

  /// Clear DataBus data for a specific dialogue within a session.
  /// Clears sys, user, and ctx namespaces for the dialogue.
  /// @param session_id The session ID
  /// @param dialogue_id The dialogue ID to clear
  /// @return Number of keys removed
  int clear_dialogue_data(const std::string& session_id, const std::string& dialogue_id);

  std::vector<std::string> get_all_session_ids() const;
  bool get_session_output(const std::string& session_id, const std::string& key,
                          DataVariant& value) const;

  bool initialize();
  bool start();
  bool pause();
  bool resume();
  bool stop();

  utils::storage::StorageResult save(const utils::storage::StorageConfig& config,
                                     const utils::storage::VersionInfo&   version_info);

  static utils::storage::StorageResult load(const AgentId& agent_id,
                                            const utils::storage::StorageConfig& storage_config,
                                            std::shared_ptr<MMAgent>& agent);

  static utils::storage::StorageResult delete_agent(
      const AgentId& agent_id, const utils::storage::StorageConfig& storage_config);

  static utils::storage::StorageResult list_agents(
      const utils::storage::StorageConfig& storage_config, std::vector<AgentId>& agent_ids);

  static utils::storage::StorageResult get_metadata(
      const AgentId& agent_id, const utils::storage::StorageConfig& storage_config,
      AgentMetadata& metadata);

  static utils::storage::StorageResult set_active_version(
      const AgentId& agent_id, const std::string& version,
      const utils::storage::StorageConfig& storage_config);

  static utils::storage::StorageResult get_active_version(
      const AgentId& agent_id, const utils::storage::StorageConfig& storage_config,
      std::string& version);

  static std::shared_ptr<MMAgent> create_from_package(const std::string& package_path);

  // Context callback configuration
  void set_context_callback(const ContextCallbackConfig& config);
  void set_context_callback(ContextCallback callback, bool async = true);
  void clear_context_callback();
  bool has_context_callback() const;

  // Session event callback for lifecycle notifications (used by runtime)
  void set_session_event_callback(SessionEventCallback callback);
  void clear_session_event_callback();

  // Dialogue event callback for lifecycle notifications (used by runtime)
  void set_dialogue_event_callback(DialogueEventCallback callback);
  void clear_dialogue_event_callback();
  bool has_dialogue_event_callback() const;

  // Task event callback for lifecycle notifications (used by runtime)
  void set_task_event_callback(TaskEventCallback callback);
  void clear_task_event_callback();
  bool has_task_event_callback() const;

 private:
  // Friend for unit test access to handle_dialogue_interrupt and create_session internals.
  friend class ::agentflow::tests::DialogueInterruptFlagFixture;
  friend class BuiltinDispatcher;

  // P0-2 修复：抽不发事件版，让 handle_session_destroy 控制事件时机
  // （先清 tasks/dialogues 再发事件，避免订阅方收到事件时状态未完全销毁）
  int clear_session_data_internal(const std::string& session_id, bool emit_event);

  Config config_;
  AgentState state_ = AgentState::CREATED;
  std::vector<std::shared_ptr<MMWorkflow>> workflows_;
  std::map<WorkflowId, std::shared_ptr<MMWorkflow>> workflow_map_;

  DataMap            data_bus_;
  runtime::MMDataBus* runtime_databus_ = nullptr;  // Pointer to Runtime's MMDataBus for unified storage
  mutable std::mutex mutex_;

  std::map<std::string, std::shared_ptr<MMAgentSession>> sessions_;
  std::shared_ptr<MMAgentTask>  current_task_;
  std::shared_ptr<AgentContext> context_;
  std::shared_ptr<AgentPackage> package_;
  std::shared_ptr<SessionRouter> session_router_;

  bool initialize_components(std::shared_ptr<AgentPackage> package = nullptr);
  std::vector<AgentPackage::TaskDefinition> build_default_session_tasks() const;
  std::shared_ptr<MMAgentSession> create_session_from_tasks(
      const std::string& session_id, const std::vector<AgentPackage::TaskDefinition>& session_tasks);
  std::shared_ptr<MMAgentSession> create_session_with_defaults(const std::string& session_id);
  std::string resolve_workflow_reference(const std::string& workflow_ref) const;
  std::string resolve_workflow_file(const std::string& workflow_ref) const;
  void        handle_routed_request_message(const std::shared_ptr<MMAgentSession>& session,
                                            const MMAgentMsgQue::Message& message);
  void handle_control_message(const std::shared_ptr<MMAgentSession>& session,
                              const MMAgentMsgQue::Message& message);
  void handle_session_interrupt(const std::shared_ptr<MMAgentSession>& session,
                                const MMAgentMsgQue::Message& message);
  void handle_session_destroy(const std::shared_ptr<MMAgentSession>& session,
                              const MMAgentMsgQue::Message& message);
  void handle_dialogue_interrupt(const std::shared_ptr<MMAgentSession>& session,
                                 const MMAgentMsgQue::Message& message);
  void handle_task_intent_request(const std::shared_ptr<MMAgentSession>& session,
                                  const std::shared_ptr<MMAgentTask>& task,
                                  const MMAgentMsgQue::Message& message);
  void handle_task_data_exchange(const std::shared_ptr<MMAgentSession>& session,
                                 const std::shared_ptr<MMAgentTask>& task,
                                 const MMAgentMsgQue::Message& message);
  std::shared_ptr<MMAgentTask> build_runtime_task(const std::shared_ptr<MMAgentSession>& session,
                                                  const std::shared_ptr<MMAgentTask>& source_task,
                                                  const MMAgentMsgQue::Message& message);

  // Skill task launching helper (used by builtin dispatcher and coordinator)
  bool launch_skill_task(const std::shared_ptr<MMAgentSession>& session,
                         const std::string& skill_name,
                         const std::string& exec_skill_key,
                         const MMAgentMsgQue::Message& context,
                         const std::shared_ptr<MMAgentTask>& source_task);

  // Result structure for next_skills processing
  struct NextSkillsLaunchResult {
    bool found_next_skills = false;     // Whether next_skills was found in databus
    bool launched_any = false;          // Whether any skills were launched
    bool all_launched = false;          // Whether all skills in next_skills are now launched
    std::set<std::string> launched_skills;  // Updated set of launched skill names
    int launched_count = 0;             // Number of skills launched in this call
  };

  struct HarnessBatchSkill {
    std::string name;
    std::string exec_skill_key;
  };

  struct HarnessBatchState {
    std::string session_id;
    std::string dialogue_id;
    std::string batch_id;
    std::string source_task_id;
    std::vector<HarnessBatchSkill> dispatch_order;
    std::set<std::string> expected_exec_keys;
    std::set<std::string> queued_exec_keys;
    std::set<std::string> active_exec_keys;
    std::map<std::string, MMAgentMsgQue::Message> terminal_results;
    MMAgentMsgQue::Message launch_context;
    int max_concurrent_skills = 1;
    bool coordinator_triggered = false;
  };

  // Unified next_skills processing (used by both UNIFIED and COORDINATOR sections)
  // Fetches next_skills from databus with selected_skills backward compat
  NextSkillsLaunchResult process_next_skills_launch(
      const std::shared_ptr<MMAgentSession>& session,
      const std::shared_ptr<MMAgentTask>& task,
      const std::string& dialogue_id,
      const MMAgentMsgQue::Message& message,
      const std::string& log_prefix);

  NextSkillsLaunchResult process_harness_batch_launch(
      const std::shared_ptr<MMAgentSession>& session,
      const std::shared_ptr<MMAgentTask>& task,
      const std::string& dialogue_id,
      const std::string& batch_id,
      const std::vector<HarnessBatchSkill>& skills,
      const MMAgentMsgQue::Message& message,
      const std::string& log_prefix);
  int pump_harness_batch(const std::shared_ptr<MMAgentSession>& session,
                         const std::string& batch_state_key);
  bool handle_harness_batch_terminal(
      const std::shared_ptr<MMAgentSession>& session,
      const std::shared_ptr<MMAgentTask>& task,
      const MMAgentMsgQue::Message& message);
  bool can_finalize_harness_dialogue(const std::string& session_id,
                                     const std::string& dialogue_id,
                                     std::string* details = nullptr);
  void interrupt_queued_harness_skills(
      const std::shared_ptr<MMAgentSession>& session);
  void clear_harness_batches(const std::string& session_id,
                             const std::string& dialogue_id = "");

  std::map<std::string, HarnessBatchState> harness_batches_;
  std::mutex                               harness_batches_mutex_;

  std::queue<std::shared_ptr<MMAgentTask>> generated_tasks_;
  std::mutex                               generated_tasks_mutex_;
  std::atomic<bool>                        message_threads_running_{false};
  std::thread                              intent_request_thread_;
  std::thread                              data_exchange_thread_;

  // Builtin dispatcher instances per session
  std::map<std::string, std::shared_ptr<BuiltinDispatcher>> builtin_dispatchers_;
  std::mutex                               builtin_dispatchers_mutex_;

  int  process_intent_request_messages();
  int  process_data_exchange_messages();
  void process_intent_request_loop();
  void process_data_exchange_loop();

  // Context callback configuration
  ContextCallbackConfig context_callback_config_;
  mutable std::mutex   context_callback_mutex_;

  // Session event callback for lifecycle notifications
  SessionEventCallback session_event_callback_;

  // Dialogue event callback for lifecycle notifications
  DialogueEventCallback dialogue_event_callback_;

  // Task event callback for lifecycle notifications
  TaskEventCallback task_event_callback_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_AGENT_H
