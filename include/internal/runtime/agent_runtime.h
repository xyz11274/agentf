#ifndef AGENTFLOW_RUNTIME_AGENT_RUNTIME_H
#define AGENTFLOW_RUNTIME_AGENT_RUNTIME_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "agent/common.h"
#include "agent/agent.h"
#include "runtime/master/deployer.h"
#include "runtime/master/scheduler.h"
#include "runtime/shared_runtime_state.h"
#include "runtime/databus/databus.h"
#include "runtime/worker.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace runtime {

using AgentPtr    = std::shared_ptr<framework::Agent>;
using WorkflowPtr = std::shared_ptr<framework::Workflow>;
using NodePtr     = std::shared_ptr<framework::Node>;

/**
 * AgentRuntime is the core scheduling and execution engine
 * Three-level Master-Worker architecture:
 * - Agent Level Master: Manages multiple agent instances
 * - Workflow Level Master: Manages workflow graph and scheduling
 * - Node Level Worker: Executes node tasks
 */
class AgentRuntime {
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
    int         worker_event_threads = 1;
    size_t      worker_queue_size = 1024;
  };

  AgentRuntime();
  explicit AgentRuntime(const Config& config);
  ~AgentRuntime();

  // Agent management (Agent Level Master)
  bool                  register_agent(AgentPtr agent);
  bool                  unregister_agent(const AgentId& agent_id);
  AgentPtr              get_agent(const AgentId& agent_id);
  std::vector<AgentPtr> list_agents();

  // Workflow scheduling (Workflow Level Master)
  bool load_workflow(const AgentId& agent_id, WorkflowPtr workflow);
  bool unload_workflow(const AgentId& agent_id, const WorkflowId& workflow_id);
  bool execute_workflow(const AgentId& agent_id, const WorkflowId& workflow_id,
                        const DataMap& inputs);

  // Node execution (Node Level Worker)
  void process_node(AgentPtr agent, WorkflowPtr workflow, NodePtr node);

  // Event handling
  void on_node_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                        const NodeId& node_id, bool success);
  void on_workflow_complete(const AgentId& agent_id, const WorkflowId& workflow_id, bool success);

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
  bool is_running() const { return is_running_; }
  RuntimeRole get_role() const { return role_; }

  // Runtime role and global deployment/scheduling
  void set_role(RuntimeRole role);

  bool register_worker_engine(const std::shared_ptr<WorkerEngine>& worker);
  bool unregister_worker_engine(const std::string& worker_id);
  bool heartbeat_worker(const std::string& worker_id);
  bool update_worker_engine_status(const std::string& worker_id, WorkerEngine::WorkerStatus status);
  std::vector<master::WorkerSnapshot> list_worker_engines() const;

  bool deploy_workflow_instance(const WorkflowInstanceInfo& info, const std::string& preferred_worker_id = "");
  bool destroy_workflow_instance(const std::string& workflow_instance_id);
  master::TaskScheduleResult schedule_task(const master::TaskScheduleRequest& request);
  bool report_agent_heartbeat(const AgentId& agent_id);
  bool update_task_status(const std::string& task_id, const std::string& status, int progress);

  // Shared resource access
  DataBus& get_databus(const AgentId& agent_id);

private:
  Config                                              config_;
  RuntimeRole                                         role_ = RuntimeRole::MASTER;
  std::shared_ptr<WorkerEngine>                       local_worker_;
  std::shared_ptr<IRuntimeSharedState>                shared_state_;
  std::unique_ptr<master::Deployer>                   deployer_;
  std::unique_ptr<master::Scheduler>                  scheduler_;

  std::map<AgentId, AgentPtr>                          agents_;
  std::map<AgentId, std::unique_ptr<DataBus>>          databuses_;
  mutable std::mutex                                   agents_mutex_;

  // Execution thread pool for nodes
  int                      num_worker_threads_ = 4;
  std::vector<std::thread> worker_threads_;
  bool                     is_running_ = false;
  std::atomic<bool>        draining_{false};

  // Node task queue
  std::queue<std::pair<AgentPtr, NodePtr>> node_task_queue_;
  std::mutex                               task_queue_mutex_;

  // Worker load tracking for load balancing
  std::vector<int> worker_loads_;
  std::mutex       worker_load_mutex_;

  // Workflow execution state tracking
  struct WorkflowExecutionState {
    WorkflowId             workflow_id;
    AgentId                agent_id;
    std::map<NodeId, bool> completed_nodes;
    std::map<NodeId, bool> failed_nodes;
    int                    total_nodes     = 0;
    int                    completed_count = 0;
    bool                   in_progress     = false;
  };
  std::map<std::string, WorkflowExecutionState> workflow_states_;
  std::mutex                                    workflow_state_mutex_;

  // Worker thread main loop
  void worker_thread_main();

  // Agent Level Master functions
  void monitor_agents();
  void agent_registration_loop();
  void agent_monitor_loop();
  void process_agent_state_change(const AgentId& agent_id, bool is_healthy);
  void check_agent_health(const AgentId& agent_id);

  // Workflow Level Master functions
  void schedule_workflow_executable_nodes(const std::string& execution_id);
  void update_workflow_state(const std::string& execution_id, const NodeId& node_id, bool success);

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
  std::map<AgentId, int64_t>           agent_heartbeats_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_AGENT_RUNTIME_H
