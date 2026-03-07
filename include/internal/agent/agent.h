#ifndef AGENTFLOW_FRAMEWORK_AGENT_H
#define AGENTFLOW_FRAMEWORK_AGENT_H

#include <memory>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "agent/common.h"
#include "agent/msgque.h"
#include "agent/package.h"
#include "workflow/workflow.h"
#include "utils/file_storage.h"

namespace agentflow {
namespace framework {

// Forward declarations for internal agent components
class MMAgentSession;
class MMAgentTask;
class AgentContext;
class MMAgentEvent;
class AgentPackage;
class SessionTaskCoordinator;

}  // namespace framework
}  // namespace agentflow

namespace agentflow {
namespace framework {

/**
 * MMAgent metadata structure
 */
struct AgentMetadata {
  AgentId                  id;
  std::string              name;
  std::string              version;
  std::string              description;
  int64_t                  created_at;
  int64_t                  updated_at;
  std::string              author;
  std::vector<WorkflowId>  workflow_ids;
};

/**
 * MMAgent is the basic execution subject in AgentFlow
 * It manages multiple workflows with shared resources
 */
class MMAgent {
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

  explicit MMAgent(const Config& config);
  ~MMAgent();

  // Getters
  const AgentId&     get_id() const { return config_.id; }
  const std::string& get_name() const { return config_.name; }
  AgentState         get_state() const { return state_; }
  const Config&      get_config() const { return config_; }

  // Component accessors
  std::shared_ptr<MMAgentSession>   get_session() const { return default_session_; }
  std::shared_ptr<MMAgentSession>   get_session(const std::string& session_key) const;
  std::vector<std::shared_ptr<MMAgentSession>> get_sessions() const;
  std::shared_ptr<MMAgentSession>   get_default_session() const { return default_session_; }
  std::shared_ptr<MMAgentTask>      get_current_task() const { return current_task_; }
  std::shared_ptr<AgentMessages>  get_messages() const { return messages_; }
  std::shared_ptr<SessionTaskCoordinator> get_coordinator() const { return coordinator_; }
  std::shared_ptr<AgentContext>   get_context() const { return context_; }
  std::shared_ptr<MMAgentEvent>     get_event_handler() const { return event_; }
  std::shared_ptr<AgentPackage>   get_package() const { return package_; }

  // Session/message routing
  std::shared_ptr<MMAgentSession> get_or_create_session(const std::string& user_id,
                                                      const std::string& client_id);
  std::shared_ptr<MMAgentSession> get_session_by_key(const std::string& session_key) const;
  bool route_request_message(const MMAgentMsgQue::Message& message);
  int  process_session_messages(const std::string& session_key);
  int  process_all_sessions_until_idle();
  bool pop_generated_task(std::shared_ptr<MMAgentTask>& task);

  // MMWorkflow management
  void                                          add_workflow(std::shared_ptr<MMWorkflow> workflow);
  void                                          remove_workflow(const WorkflowId& workflow_id);
  std::shared_ptr<MMWorkflow>                     get_workflow(const WorkflowId& workflow_id);
  const std::vector<std::shared_ptr<MMWorkflow>>& get_workflows() const { return workflows_; }

  // Shared resource management
  bool initialize_resources();
  void cleanup_resources();

  // Data bus operations
  bool publish_data(const std::string& key, const DataVariant& value);
  bool get_data(const std::string& key, DataVariant& value);

  // Cross-session query helpers
  std::vector<std::string> get_all_session_ids() const;
  bool get_session_output(const std::string& session_id, const std::string& key,
                          DataVariant& value) const;

  // Control flow event queue
  void        post_event(const std::string& event);
  std::string get_event();

  // MMAgent lifecycle
  bool initialize();
  bool start();
  bool pause();
  bool resume();
  bool stop();

  // Storage operations
  utils::storage::StorageResult save(const utils::storage::StorageConfig& config,
                                     const utils::storage::VersionInfo&   version_info);

  static utils::storage::StorageResult load(const AgentId&                agent_id,
                                            const utils::storage::StorageConfig& storage_config,
                                            std::shared_ptr<MMAgent>&       agent);

  static utils::storage::StorageResult delete_agent(const AgentId&                agent_id,
                                                    const utils::storage::StorageConfig& storage_config);

  static utils::storage::StorageResult list_agents(const utils::storage::StorageConfig& storage_config,
                                                   std::vector<AgentId>&         agent_ids);

  static utils::storage::StorageResult get_metadata(const AgentId&                agent_id,
                                                    const utils::storage::StorageConfig& storage_config,
                                                    AgentMetadata&                metadata);

  static utils::storage::StorageResult set_active_version(const AgentId&                agent_id,
                                                          const std::string&            version,
                                                          const utils::storage::StorageConfig& storage_config);

  static utils::storage::StorageResult get_active_version(const AgentId&                agent_id,
                                                          const utils::storage::StorageConfig& storage_config,
                                                          std::string&                  version);

  // Factory method to create an agent from a package directory
  static std::shared_ptr<MMAgent> create_from_package(const std::string& package_path);

private:
  Config                                          config_;
  AgentState                                      state_ = AgentState::CREATED;
  std::vector<std::shared_ptr<MMWorkflow>>          workflows_;
  std::map<WorkflowId, std::shared_ptr<MMWorkflow>> workflow_map_;

  // Shared resources
  DataMap                 data_bus_;
  std::queue<std::string> event_queue_;
  mutable std::mutex      mutex_;
  mutable std::mutex      event_mutex_;

  // MMAgent components
  std::shared_ptr<MMAgentSession>   default_session_;
  std::map<std::string, std::shared_ptr<MMAgentSession>> sessions_;
  std::shared_ptr<MMAgentTask>      current_task_;
  std::shared_ptr<AgentMessages>  messages_;
  std::shared_ptr<SessionTaskCoordinator> coordinator_;
  std::shared_ptr<AgentContext>   context_;
  std::shared_ptr<MMAgentEvent>     event_;
  std::shared_ptr<AgentPackage>   package_;

  // Helper method to initialize components
  bool initialize_components(std::shared_ptr<AgentPackage> package = nullptr);
  AgentPackage::SessionDefinition build_default_session_definition() const;
  std::shared_ptr<MMAgentSession> create_session_from_definition(
      const std::string& session_id, const AgentPackage::SessionDefinition& session_definition);
  std::shared_ptr<MMAgentSession> create_session_with_defaults(const std::string& session_id);
  std::string resolve_workflow_reference(const std::string& workflow_ref) const;
  std::string resolve_workflow_file(const std::string& workflow_ref) const;

  std::queue<MMAgentMsgQue::Message> request_event_queue_;
  std::condition_variable           request_event_cv_;
  std::thread                       request_event_thread_;
  std::atomic<bool>                 request_event_running_{false};

  std::queue<std::shared_ptr<MMAgentTask>> generated_tasks_;
  std::mutex                             generated_tasks_mutex_;

  void request_event_loop();
  void handle_request_event(const MMAgentMsgQue::Message& message);
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_AGENT_H
