#ifndef AGENTFLOW_RUNTIME_WORKER_H
#define AGENTFLOW_RUNTIME_WORKER_H

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent/common.h"
#include "runtime/databus/databus.h"
#include "runtime/exe_loader.h"
#include "utils/thread_pool.h"

namespace agentflow {
namespace runtime {

class MMWorkerEngine {
public:
  enum class ExecutionMode {
    CPP_FUNCTION = 0,
    SHELL_SCRIPT = 1,
    EXECUTABLE = 2,
    GO_SCRIPT = 3,
    PYTHON_SCRIPT = 4,
    JAVA_JAR = 5,
    SO_LIBRARY = 6
  };

  struct ExecutionRequest {
    ExecutionMode             mode = ExecutionMode::CPP_FUNCTION;
    std::string               command;
    std::string               function_name;
    std::string               main_class;
    std::string               working_dir;
    std::vector<std::string>  args;
    int                       timeout_ms = 30000;
    DataMap                   inputs;
    std::string               databus_root_dir;
    bool                      keep_databus_files = false;
    bool                      enable_hot_reload = false;
    HotReloadConfig           hot_reload_config;
    // Injected DataBus for SO library nodes (V2 signature)
    MMDataBus*                databus = nullptr;
    // Session ID for session-isolated DataBus access (V3 signature)
    // When provided, V3 signature will be used with session isolation
    std::string               session_id;
    // Plugin UDS path - injected as AF_PLUGIN_UDS_FILE environment variable
    std::string               plugin_uds_path;
    struct LLMServiceConfig {
      std::string base_url;
      std::string api_key;
      std::string model = "gpt-4.1";
      int         timeout_ms = 30000;
    } llm_config;
  };

  struct ExecutionResponse {
    bool                      success = false;
    DataMap                   outputs;
    std::string               stdout_content;
    std::string               stderr_content;
    std::string               error_message;
    int                       exit_code = -1;
    std::string               active_version_id;
  };

  enum class WorkerStatus {
    STARTING = 0,
    RUNNING  = 1,
    DEGRADED = 2,
    STOPPED  = 3,
  };

  struct Config {
    std::string worker_id;
    bool        preconfigured = false;
    int         default_workflow_threads = 1;
    int         control_threads = 1;
    int         business_threads = 1;
    size_t      max_queue_size = 1024;
    std::string plugin_uds_path;  // Plugin UDS path to inject into node processes
  };

  enum class TaskPriority {
    NORMAL = 0,
    HIGH   = 1,
  };

  struct TaskItem {
    std::string                        task_id;
    std::string                        workflow_id;
    std::string                        workflow_instance_id;
    TaskPriority                       priority = TaskPriority::NORMAL;
    std::map<std::string, std::string> metadata;
  };

  struct WorkflowRegistration {
    std::string workflow_id;
    std::string workflow_instance_id;
    int         thread_count = 0;
  };

  struct ControlMessage {
    std::string                        type;
    std::map<std::string, std::string> data;
    int64_t                            created_at_ms = 0;
  };

  struct BusinessMessage {
    TaskItem                           task;
    std::map<std::string, std::string> data;
    int64_t                            created_at_ms = 0;
  };

  using WorkerControlHandler = std::function<void(const ControlMessage&)>;

  explicit MMWorkerEngine(const Config& config);
  ~MMWorkerEngine();

  const std::string& get_worker_id() const { return config_.worker_id; }
  WorkerStatus       get_status() const;
  int                get_load() const;
  int64_t            get_last_heartbeat_ms() const;
  bool               is_preconfigured() const { return config_.preconfigured; }

  void set_status(WorkerStatus status);
  void set_load(int load);
  void heartbeat();

  void                     set_supported_workflows(const std::set<std::string>& workflow_ids);
  void                     add_supported_workflow(const std::string& workflow_id);
  void                     remove_supported_workflow(const std::string& workflow_id);
  bool                     supports_workflow(const std::string& workflow_id) const;
  std::set<std::string>    get_supported_workflows() const;

  // DataBus injection for in-process sharing with Runtime
  void                     set_databus(const std::string& agent_id, MMDataBus* databus);
  MMDataBus*               get_databus(const std::string& agent_id) const;
  void                     remove_databus(const std::string& agent_id);

  bool start();
  void stop();
  bool is_running() const { return running_; }

  bool enqueue_task(const TaskItem& task);
  bool register_workflow_instance(const WorkflowRegistration& registration);
  bool unregister_workflow_instance(const std::string& workflow_instance_id);
  bool enqueue_control_message(const ControlMessage& message);
  bool enqueue_business_message(const BusinessMessage& message);
  void register_control_handler(const std::string& message_type, WorkerControlHandler handler);
  static ExecutionResponse execute_code(const ExecutionRequest& request);
  static bool check_and_reload_code(const std::string& command,
                                    const HotReloadConfig& config = HotReloadConfig());
  static std::shared_ptr<ExecutableVersion> get_code_version(const std::string& command);
  static bool rollback_code_version(const std::string& command, const std::string& version_id);
  static bool stop_hot_reload(const std::string& command);

private:
  static int64_t now_ms();
  std::string workflow_pool_key(const TaskItem& task) const;
  void        ensure_workflow_pool(const std::string& pool_key, int thread_count);
  void        handle_control_message(const ControlMessage& message);
  void        process_business_messages();
  void        control_message_loop();
  void        business_message_loop();

  Config                 config_;
  WorkerStatus           status_ = WorkerStatus::STARTING;
  int                    load_ = 0;
  int64_t                last_heartbeat_ms_ = 0;
  std::set<std::string>  supported_workflows_;
  mutable std::mutex     mutex_;

  // Injected DataBus collection from Runtime (for in-process sharing)
  // Key: AgentId, Value: pointer to Runtime's MMDataBus
  std::map<std::string, MMDataBus*> databuses_;
  mutable std::mutex                 databus_mutex_;

  std::atomic<bool> running_{false};

  std::deque<ControlMessage>       control_messages_;
  std::deque<BusinessMessage>      business_messages_;
  std::mutex                       queue_mutex_;
  std::condition_variable          queue_cv_;

  std::unordered_map<std::string, std::unique_ptr<basic::ThreadPool>> workflow_pools_;
  std::unique_ptr<basic::ThreadPool>                                  control_pool_;

  std::unordered_map<std::string, std::vector<WorkerControlHandler>> control_handlers_;
  std::mutex                                                         handler_mutex_;
  std::thread                                                        control_thread_;
  std::thread                                                        business_thread_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_WORKER_H
