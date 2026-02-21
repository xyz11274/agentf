#ifndef AGENTFLOW_FRAMEWORK_AGENT_H
#define AGENTFLOW_FRAMEWORK_AGENT_H

#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "agent/common.h"
#include "workflow/workflow.h"

// Forward declaration
namespace agentflow {
namespace utils {
namespace storage {
struct StorageConfig;
struct StorageResult;
struct VersionInfo;
struct StorageStats;
}  // namespace storage
}  // namespace utils
}  // namespace agentflow

namespace agentflow {
namespace framework {

struct AgentMetadata {
  std::string                        id;
  std::string                        name;
  std::string                        version;
  std::string                        description;
  int64_t                            created_at;
  int64_t                            updated_at;
  std::string                        author;
  std::vector<std::string>           workflow_ids;
  std::map<std::string, std::string> tags;
};

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

  // Storage operations
  agentflow::utils::storage::StorageResult save(const agentflow::utils::storage::StorageConfig& config,
                                         const agentflow::utils::storage::VersionInfo&   version_info);

  static agentflow::utils::storage::StorageResult load(const std::string&                       agent_id,
                                                const agentflow::utils::storage::StorageConfig& storage_config,
                                                std::shared_ptr<Agent>&                  agent);

  static agentflow::utils::storage::StorageResult delete_agent(
      const std::string&                       agent_id,
      const agentflow::utils::storage::StorageConfig& storage_config);

  static agentflow::utils::storage::StorageResult list_agents(
      const agentflow::utils::storage::StorageConfig& storage_config,
      std::vector<std::string>&                agent_ids);

  static agentflow::utils::storage::StorageResult get_metadata(
      const std::string&                       agent_id,
      const agentflow::utils::storage::StorageConfig& storage_config,
      AgentMetadata&                           metadata);

  static agentflow::utils::storage::StorageResult set_active_version(
      const AgentId&                agent_id,
      const std::string&            version,
      const agentflow::utils::storage::StorageConfig& storage_config);

  static agentflow::utils::storage::StorageResult get_active_version(
      const AgentId&                agent_id,
      const agentflow::utils::storage::StorageConfig& storage_config,
      std::string&                  version);

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
