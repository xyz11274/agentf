#ifndef AGENTFLOW_RUNTIME_AGENT_RUNTIME_H
#define AGENTFLOW_RUNTIME_AGENT_RUNTIME_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <vector>

#include "agent/common.h"
#include "agent/agent.h"
#include "runtime/event_handler.h"
#include "runtime/master/deployer.h"
#include "runtime/master/scheduler.h"
#include "runtime/shared_runtime_state.h"
#include "runtime/databus/databus.h"
#include "runtime/databus/uds_server.h"
#include "runtime/worker.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace runtime {

using AgentPtr    = std::shared_ptr<framework::MMAgent>;
using WorkflowPtr = std::shared_ptr<framework::MMWorkflow>;
using NodePtr     = std::shared_ptr<framework::MMNode>;

/**
 * MMAgentRuntime is the core scheduling and execution engine
 * Three-level Master-Worker architecture:
 * - MMAgent Level Master: Manages multiple agent instances
 * - MMWorkflow Level Master: Manages workflow graph and scheduling
 * - MMNode Level Worker: Executes node tasks
 */
class MMAgentRuntime {
public:
  enum class RuntimeRole {
    WORKER = 0,
    MASTER = 1,  // MASTER includes WORKER capabilities.
  };

  struct Config {
    RuntimeRole role = RuntimeRole::MASTER;
    int         worker_threads = 4;
    std::string local_worker_id = "runtime-local-worker";
    int         worker_default_workflow_threads = 1;
    int         worker_control_threads = 1;
    int         worker_business_threads = 1;
    size_t      worker_queue_size = 1024;
    std::string plugin_uds_path;  // Plugin UDS path to inject into node processes
  };

  MMAgentRuntime();
  explicit MMAgentRuntime(const Config& config);
  ~MMAgentRuntime();

  // MMAgent management (MMAgent Level Master)
  bool                  register_agent(AgentPtr agent);
  bool                  unregister_agent(const AgentId& agent_id);
  AgentPtr              get_agent(const AgentId& agent_id);
  std::vector<AgentPtr> list_agents();

  // MMWorkflow scheduling (MMWorkflow Level Master)
  bool load_workflow(const AgentId& agent_id, WorkflowPtr workflow);
  bool unload_workflow(const AgentId& agent_id, const WorkflowId& workflow_id);
  bool execute_workflow(const AgentId& agent_id, const WorkflowId& workflow_id,
                        const DataMap& inputs, const std::string& source_task_id = "",
                        const std::string& session_id = "", const std::string& dialogue_id = "",
                        const std::string& trace_labels = "",
                        const std::string& trace_id = "",
                        const std::string& task_type = "", const std::string& skill_task_id = "",
                        const std::string& skill_name = "");

  // MMNode execution (MMNode Level Worker)
  void process_node(AgentPtr agent, WorkflowPtr workflow, NodePtr node, const std::string& execution_id);

  // Event handling
  void on_node_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                        const NodeId& node_id, bool success);
  void on_workflow_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                            const std::string& execution_id, bool success);

  // Load balancing
  void update_worker_load(int worker_id, int load_change);
  int  get_least_loaded_worker();

  // Runtime statistics
  int get_active_agents() const;
  int get_active_workflows() const;
  int get_running_nodes() const;

  // Lifecycle management
  bool start();
  bool stop();
  bool is_running() const { return is_running_.load(); }
  RuntimeRole get_role() const { return role_; }

  // Runtime role and global deployment/scheduling
  void set_role(RuntimeRole role);

  bool register_worker_engine(const std::shared_ptr<MMWorkerEngine>& worker);
  bool unregister_worker_engine(const std::string& worker_id);
  bool heartbeat_worker(const std::string& worker_id);
  bool update_worker_engine_status(const std::string& worker_id, MMWorkerEngine::WorkerStatus status);
  std::vector<master::WorkerSnapshot> list_worker_engines() const;

  bool deploy_workflow_instance(const WorkflowInstanceInfo& info, const std::string& preferred_worker_id = "");
  bool destroy_workflow_instance(const std::string& workflow_instance_id);
  master::TaskScheduleResult schedule_task(const master::TaskScheduleRequest& request);
  bool report_agent_heartbeat(const AgentId& agent_id);
  bool update_task_status(const std::string& task_id, const std::string& status, int progress);
  std::vector<WorkflowInstanceInfo> list_workflow_instances() const;
  std::map<std::string, master::TaskScheduleResult> list_task_assignments() const;

  // Shared resource access
  MMDataBus& get_databus(const AgentId& agent_id);

  // DataBus cleanup functions
  /// Clear all DataBus data for a session.
  /// @param agent_id The agent ID
  /// @param session_id The session ID to clear
  /// @return Number of keys removed, or -1 if agent not found
  int clear_session_data(const AgentId& agent_id, const std::string& session_id);

  /// Clear DataBus data for a specific dialogue within a session.
  /// @param agent_id The agent ID
  /// @param session_id The session ID
  /// @param dialogue_id The dialogue ID to clear
  /// @return Number of keys removed, or -1 if agent not found
  int clear_dialogue_data(const AgentId& agent_id,
                           const std::string& session_id,
                           const std::string& dialogue_id);

  // AsyncCall SDK support
  /// Send an async message to a target task via standard routing.
  /// Used by SendMsg() for cross-task communication.
  bool send_async_message(const framework::MMAgentMsgQue::Message& message);

  /// Enqueue an async callback message directly to the node execution queue.
  /// Used by SendToSelf() for short-circuit routing.
  bool enqueue_async_callback(const framework::MMAgentMsgQue::Message& message);

  // Event handler management (Master only)
  void register_event_handler(std::shared_ptr<IEventHandler> handler);
  void unregister_event_handler(std::shared_ptr<IEventHandler> handler);
  void clear_event_handlers();
  size_t get_event_handler_count() const;
  void set_callback_thread_pool_size(size_t size);
  size_t get_callback_thread_pool_size() const;

private:
  Config                                              config_;
  RuntimeRole                                         role_ = RuntimeRole::MASTER;
  std::shared_ptr<MMWorkerEngine>                       local_worker_;
  std::shared_ptr<IRuntimeSharedState>                shared_state_;
  std::unique_ptr<master::Deployer>                   deployer_;
  std::unique_ptr<master::Scheduler>                  scheduler_;

  std::map<AgentId, AgentPtr>                          agents_;
  std::map<AgentId, std::unique_ptr<MMDataBus>>          databuses_;
  mutable std::mutex                                   agents_mutex_;

  // Execution thread pool for nodes
  int                      num_worker_threads_ = 4;
  std::vector<std::thread> worker_threads_;
  std::atomic<bool>        is_running_{false};
  std::atomic<bool>        draining_{false};

  // MMNode task queue
  struct QueuedNodeTask {
    AgentPtr    agent;
    WorkflowPtr workflow;
    NodePtr     node;
    std::string execution_id;
  };
  std::queue<QueuedNodeTask> node_task_queue_;
  std::mutex                 task_queue_mutex_;

  // Worker load tracking for load balancing
  std::vector<int> worker_loads_;
  std::mutex       worker_load_mutex_;

  // MMWorkflow execution state tracking
  struct WorkflowExecutionState {
     WorkflowId             workflow_id;
     AgentId                agent_id;
     std::string            source_task_id;    // Task that initiated this workflow (for tracing/response routing)
     std::string            session_id;        // Session ID for response routing
     std::string            dialogue_id;       // Dialogue ID for multi-dialogue support
     std::string            trace_labels;      // Labels for flow trace correlation (from message.metadata["trace_labels"])
     std::string            trace_id;           // Correlation id for end-to-end tracing
     std::string            task_type;         // Type of the task executing this workflow (skill, dispatcher, coordinator)
     std::string            skill_task_id;     // Actual task id of the skill (for skill completion tracking)
     std::string            skill_name;        // Skill name (for skill completion notification)
     DataMap                inputs;            // Workflow inputs (passed via state, not databus, for concurrency isolation)
     std::map<NodeId, bool> completed_nodes;
     std::map<NodeId, bool> failed_nodes;
     std::set<NodeId>       queued_nodes;
     std::set<NodeId>       running_nodes;
     int                    total_nodes     = 0;
     int                    completed_count = 0;
     bool                   in_progress     = false;
     std::string            failure_details;   // Failed node error/stderr for coordinator judgment

     // UDS server for DataBus access - lifecycle tied to workflow execution
     std::string            uds_socket_path;     // Socket file path
     std::shared_ptr<UDSServer> uds_server;      // UDS server instance

     // Async operation tracking - shared_ptr to atomic for thread-safe access from async threads
     std::shared_ptr<std::atomic<int>> async_pending_count = std::make_shared<std::atomic<int>>(0);
     bool                   has_async_pending() const { return async_pending_count && async_pending_count->load() > 0; }
     void                   increment_async_pending() { if(async_pending_count) async_pending_count->fetch_add(1); }
     void                   decrement_async_pending() {
       if(async_pending_count) {
         int expected = async_pending_count->load();
         while (expected > 0 && !async_pending_count->compare_exchange_weak(expected, expected - 1)) {
           // CAS loop for thread-safe decrement
         }
       }
     }
     // Pending callback count per node (for handling race condition where callbacks arrive before ASYNC_PENDING)
     std::map<std::string, int> pending_callbacks;
   };
  std::map<std::string, WorkflowExecutionState> workflow_states_;
  std::mutex                                    workflow_state_mutex_;

  // Cloned workflows for concurrent execution isolation
  // Each execution gets its own workflow copy with independent node states
  std::map<std::string, WorkflowPtr>            cloned_workflows_;
  std::mutex                                    cloned_workflows_mutex_;

  // Event handlers (Master only)
  std::vector<std::shared_ptr<IEventHandler>>   event_handlers_;
  mutable std::mutex                            event_handlers_mutex_;
  size_t                                        callback_thread_pool_size_ = 5;

  // Callback thread pool for async event execution
  std::vector<std::thread>                       callback_threads_;
  std::queue<std::function<void()>>              callback_queue_;
  std::mutex                                     callback_queue_mutex_;
  std::condition_variable                        callback_queue_cv_;
  std::atomic<bool>                              callback_threads_running_{false};

  // Worker thread main loop
  void worker_thread_main();

  // MMAgent Level Master functions
  void monitor_agents();
  void agent_registration_loop();
  void agent_monitor_loop();
  // Polls each registered agent for generated tasks (from msgque processing) and
  // dispatches them to execute_workflow().  Runs on a dedicated background thread.
  void agent_task_poller_loop();
  void process_agent_state_change(const AgentId& agent_id, bool is_healthy);
  void check_agent_health(const AgentId& agent_id);
  void initialize_agent_workflow_pools(const AgentPtr& agent);
  bool ensure_workflow_pool(const std::string& agent_id, const std::string& task_owner_id,
                            const std::string& workflow_id, const std::string& workflow_file,
                            int workflow_pool_size);

  // Event emission methods
  void emit_agent_event(const AgentEvent& event);
  void emit_session_event(const SessionEvent& event);
  void emit_dialogue_event(const DialogueEvent& event);
  void emit_task_event(const TaskEvent& event);
  std::vector<WorkflowInfo> build_workflow_infos(const AgentPtr& agent);

  // Callback thread pool methods
  void start_callback_threads();
  void stop_callback_threads();
  void callback_thread_main();

  // MMWorkflow Level Master functions
  void schedule_workflow_executable_nodes(const std::string& execution_id);
  void update_workflow_state(const std::string& execution_id, const NodeId& node_id, bool success,
                             const std::string& failure_details = "");

  struct AgentRegistrationRequest {
    AgentPtr                 agent;
    bool                     unregister = false;
    std::condition_variable* done_cv = nullptr;
    std::mutex*              done_mutex = nullptr;
    bool*                    done = nullptr;
    bool*                    result = nullptr;
  };
  std::queue<AgentRegistrationRequest> agent_registration_queue_;
  std::mutex                           agent_registration_mutex_;
  std::condition_variable              agent_registration_cv_;
  std::thread                          agent_registration_thread_;
  std::thread                          agent_monitor_thread_;
  std::thread                          agent_task_poller_thread_;
  std::map<AgentId, int64_t>           agent_heartbeats_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_AGENT_RUNTIME_H
