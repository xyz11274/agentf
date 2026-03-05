#ifndef AGENTFLOW_RUNTIME_WORKER_H
#define AGENTFLOW_RUNTIME_WORKER_H

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
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
    bool                      enable_hot_reload = false;
    HotReloadConfig           hot_reload_config;
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
    int         event_threads = 1;
    size_t      max_queue_size = 1024;
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

  struct WorkerEvent {
    std::string                        type;
    std::map<std::string, std::string> data;
    int64_t                            created_at_ms = 0;
  };

  using WorkerEventHandler = std::function<void(const WorkerEvent&)>;

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

  bool start();
  void stop();
  bool is_running() const { return running_; }

  bool enqueue_task(const TaskItem& task);
  bool register_workflow_instance(const WorkflowRegistration& registration);
  bool unregister_workflow_instance(const std::string& workflow_instance_id);
  bool enqueue_event(const WorkerEvent& event);
  void register_event_handler(const std::string& event_type, WorkerEventHandler handler);
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
  void        handle_internal_event(const WorkerEvent& event);
  void        process_task_queues();
  void        event_monitor_loop();

  Config                 config_;
  WorkerStatus           status_ = WorkerStatus::STARTING;
  int                    load_ = 0;
  int64_t                last_heartbeat_ms_ = 0;
  std::set<std::string>  supported_workflows_;
  mutable std::mutex     mutex_;

  std::atomic<bool> running_{false};

  std::deque<TaskItem>             high_priority_tasks_;
  std::deque<TaskItem>             normal_priority_tasks_;
  std::queue<WorkflowRegistration> workflow_registrations_;
  std::queue<WorkerEvent>          event_queue_;
  std::mutex                       queue_mutex_;
  std::condition_variable          queue_cv_;

  std::unordered_map<std::string, std::unique_ptr<basic::ThreadPool>> workflow_pools_;
  std::unique_ptr<basic::ThreadPool>                                  event_pool_;

  std::unordered_map<std::string, std::vector<WorkerEventHandler>> event_handlers_;
  std::mutex                                                      handler_mutex_;
  std::thread                                                     event_monitor_thread_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_WORKER_H
