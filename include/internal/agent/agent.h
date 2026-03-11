#ifndef AGENTFLOW_FRAMEWORK_AGENT_H
#define AGENTFLOW_FRAMEWORK_AGENT_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "agent/common.h"
#include "agent/msgque.h"
#include "agent/package.h"
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
class BuiltinDispatcher;

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
    int data_bus_capacity = 1024 * 1024;
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

  std::shared_ptr<MMAgentSession> get_or_create_session(const std::string& user_id,
                                                        const std::string& client_id);
  std::shared_ptr<MMAgentSession> get_session_by_key(const std::string& session_key) const;
  bool route_request_message(const MMAgentMsgQue::Message& message);
  bool dequeue_response_message(MMAgentMsgQue::Message& message);
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

 private:
  Config config_;
  AgentState state_ = AgentState::CREATED;
  std::vector<std::shared_ptr<MMWorkflow>> workflows_;
  std::map<WorkflowId, std::shared_ptr<MMWorkflow>> workflow_map_;

  DataMap            data_bus_;
  mutable std::mutex mutex_;

  std::map<std::string, std::shared_ptr<MMAgentSession>> sessions_;
  std::shared_ptr<MMAgentTask>  current_task_;
  std::shared_ptr<AgentContext> context_;
  std::shared_ptr<AgentPackage> package_;
  std::shared_ptr<SessionRouter> session_router_;

  bool initialize_components(std::shared_ptr<AgentPackage> package = nullptr);
  AgentPackage::SessionDefinition build_default_session_definition() const;
  std::shared_ptr<MMAgentSession> create_session_from_definition(
      const std::string& session_id, const AgentPackage::SessionDefinition& session_definition);
  std::shared_ptr<MMAgentSession> create_session_with_defaults(const std::string& session_id);
  std::string resolve_workflow_reference(const std::string& workflow_ref) const;
  std::string resolve_workflow_file(const std::string& workflow_ref) const;
  void        handle_routed_request_message(const std::shared_ptr<MMAgentSession>& session,
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
                         const MMAgentMsgQue::Message& context,
                         const std::shared_ptr<MMAgentTask>& source_task);

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
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_AGENT_H
