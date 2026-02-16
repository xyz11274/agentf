#ifndef AGENTFLOW_RUNTIME_AGENT_RUNTIME_H
#define AGENTFLOW_RUNTIME_AGENT_RUNTIME_H

#include "agentflow/common.h"
#include "agentflow/runtime/databus.h"
#include "agentflow/framework/agent.h"
#include <thread>
#include <queue>
#include <mutex>
#include <memory>
#include <vector>

namespace agentflow {
namespace runtime {

using AgentPtr = std::shared_ptr<framework::Agent>;
using WorkflowPtr = std::shared_ptr<framework::Workflow>;
using NodePtr = std::shared_ptr<framework::Node>;

/**
 * AgentRuntime is the core scheduling and execution engine
 * Three-level Master-Worker architecture:
 * - Agent Level Master: Manages multiple agent instances
 * - Workflow Level Master: Manages workflow DAG and scheduling
 * - Node Level Worker: Executes node tasks
 */
class AgentRuntime {
public:
    AgentRuntime();
    ~AgentRuntime();
    
    // Agent management (Agent Level Master)
    bool register_agent(AgentPtr agent);
    bool unregister_agent(const AgentId& agent_id);
    AgentPtr get_agent(const AgentId& agent_id);
    std::vector<AgentPtr> list_agents();
    
    // Workflow scheduling (Workflow Level Master)
    bool load_workflow(const AgentId& agent_id, WorkflowPtr workflow);
    bool unload_workflow(const AgentId& agent_id, const WorkflowId& workflow_id);
    bool execute_workflow(const AgentId& agent_id, const WorkflowId& workflow_id, const DataMap& inputs);
    
    // Node execution (Node Level Worker)
    void process_node(AgentPtr agent, WorkflowPtr workflow, NodePtr node);

    // Event handling
    void on_node_complete(const AgentId& agent_id, const WorkflowId& workflow_id, const NodeId& node_id, bool success);
    void on_workflow_complete(const AgentId& agent_id, const WorkflowId& workflow_id, bool success);

    // Load balancing
    void update_worker_load(int worker_id, int load_change);
    int get_least_loaded_worker();
    
    // Runtime statistics
    int get_active_agents() const;
    int get_active_workflows() const;
    int get_running_nodes() const;
    
    // Lifecycle management
    bool start();
    bool stop();
    bool is_running() const { return is_running_; }
    
    // Shared resource access
    HighSpeedDataBus& get_databus(const AgentId& agent_id);
    
private:
    std::map<AgentId, AgentPtr> agents_;
    std::map<AgentId, std::unique_ptr<HighSpeedDataBus>> databuses_;
    mutable std::mutex agents_mutex_;

    // Execution thread pool for nodes
    int num_worker_threads_ = 4;
    std::vector<std::thread> worker_threads_;
    bool is_running_ = false;

    // Node task queue
    std::queue<std::pair<AgentPtr, NodePtr>> node_task_queue_;
    std::mutex task_queue_mutex_;

    // Worker load tracking for load balancing
    std::vector<int> worker_loads_;
    std::mutex worker_load_mutex_;

    // Workflow execution state tracking
    struct WorkflowExecutionState {
        WorkflowId workflow_id;
        AgentId agent_id;
        std::map<NodeId, bool> completed_nodes;
        std::map<NodeId, bool> failed_nodes;
        int total_nodes = 0;
        int completed_count = 0;
        bool in_progress = false;
    };
    std::map<std::string, WorkflowExecutionState> workflow_states_;
    std::mutex workflow_state_mutex_;

    // Worker thread main loop
    void worker_thread_main();

    // Agent Level Master functions
    void monitor_agents();
    void check_agent_health(const AgentId& agent_id);

    // Workflow Level Master functions
    void schedule_workflow_executable_nodes(const std::string& execution_id);
    void update_workflow_state(const std::string& execution_id, const NodeId& node_id, bool success);
};

} // namespace runtime
} // namespace agentflow

#endif // AGENTFLOW_RUNTIME_AGENT_RUNTIME_H
