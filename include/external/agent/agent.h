#ifndef AGENTFLOW_FRAMEWORK_AGENT_H
#define AGENTFLOW_FRAMEWORK_AGENT_H

#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "common.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace framework {

/**
 * Agent is the basic execution subject in AgentFlow
 * It manages multiple workflows with shared resources
 */
class Agent {
public:
  enum class LifecyclePolicy {
    PER_REQUEST = 0,  // Create new instance for each request
    FACTORY     = 1,  // Reuse instances from a pool
    SINGLETON   = 2,  // Single global instance
  };

  struct Config {
    AgentId     id;
    std::string name;
    std::string version = "1.0";
    std::string description;

    // Lifecycle policy
    LifecyclePolicy policy = LifecyclePolicy::PER_REQUEST;

    // Resource definition
    int max_workflows            = 10;
    int max_concurrent_workflows = 5;
    int data_bus_capacity        = 1024 * 1024;  // 1MB
  };

  explicit Agent(const Config& config);
  ~Agent() = default;

  // Getters
  const AgentId&     get_id() const { return config_.id; }
  const std::string& get_name() const { return config_.name; }
  AgentState         get_state() const { return state_; }
  const Config&      get_config() const { return config_; }

  // Workflow management
  void                                          add_workflow(std::shared_ptr<Workflow> workflow);
  void                                          remove_workflow(const WorkflowId& workflow_id);
  std::shared_ptr<Workflow>                     get_workflow(const WorkflowId& workflow_id);
  const std::vector<std::shared_ptr<Workflow>>& get_workflows() const { return workflows_; }

  // Shared resource management
  bool initialize_resources();
  void cleanup_resources();

  // Data bus operations
  bool publish_data(const std::string& key, const DataVariant& value);
  bool get_data(const std::string& key, DataVariant& value);

  // Control flow event queue
  void        post_event(const std::string& event);
  std::string get_event();

  // Agent lifecycle
  bool initialize();
  bool start();
  bool pause();
  bool resume();
  bool stop();

private:
  Config                                          config_;
  AgentState                                      state_ = AgentState::CREATED;
  std::vector<std::shared_ptr<Workflow>>          workflows_;
  std::map<WorkflowId, std::shared_ptr<Workflow>> workflow_map_;

  // Shared resources
  DataMap                 data_bus_;
  std::queue<std::string> event_queue_;
  mutable std::mutex      mutex_;
  mutable std::mutex      event_mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_AGENT_H
