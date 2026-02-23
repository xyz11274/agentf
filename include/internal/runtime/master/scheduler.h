#ifndef AGENTFLOW_RUNTIME_MASTER_SCHEDULER_H
#define AGENTFLOW_RUNTIME_MASTER_SCHEDULER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "runtime/shared_runtime_state.h"

namespace agentflow {
namespace runtime {
namespace master {

struct TaskScheduleRequest {
  std::string task_id;
  std::string agent_object_id;
  std::string agent_instance_id;
  std::string workflow_id;
};

struct TaskScheduleResult {
  bool        scheduled = false;
  std::string workflow_instance_id;
  std::string worker_id;
};

class Scheduler {
public:
  struct Config {
    int         schedule_interval_ms = 100;
    std::string plugin_so_path;
  };

  using SchedulerAlgorithm = std::function<TaskScheduleResult(
      const TaskScheduleRequest&, const std::vector<WorkflowInstanceInfo>&,
      const std::map<std::string, TaskScheduleResult>&)>;

  Scheduler();
  explicit Scheduler(const Config& config);
  ~Scheduler();

  TaskScheduleResult schedule_task(const TaskScheduleRequest& request,
                                   const std::vector<WorkflowInstanceInfo>& workflow_instances);
  bool               enqueue_task(const TaskScheduleRequest& request);
  bool               start();
  void               stop();
  bool               is_running() const { return running_; }
  bool               load_plugin_algorithm(const std::string& so_path);
  void               set_workflow_snapshot_provider(
      std::function<std::vector<WorkflowInstanceInfo>()> provider);
  std::map<std::string, TaskScheduleResult> list_task_assignments() const;
  bool handle_agent_state_change(const std::string& agent_instance_id, bool is_healthy);
  bool update_task_status(const std::string& task_id, const std::string& status, int progress);

private:
  TaskScheduleResult run_default_algorithm(const TaskScheduleRequest& request,
                                           const std::vector<WorkflowInstanceInfo>& workflow_instances,
                                           const std::map<std::string, TaskScheduleResult>& stable_mapping) const;
  void scheduling_loop();

  Config config_;
  std::atomic<bool> running_{false};
  std::thread       scheduling_thread_;
  std::queue<TaskScheduleRequest> task_queue_;
  mutable std::mutex              queue_mutex_;
  std::condition_variable         queue_cv_;

  mutable std::mutex                             mapping_mutex_;
  std::map<std::string, TaskScheduleResult>      task_to_workflow_;
  std::map<std::string, std::string>             task_status_;
  std::map<std::string, int>                     workflow_task_load_;
  std::function<std::vector<WorkflowInstanceInfo>()> workflow_snapshot_provider_;

  SchedulerAlgorithm algorithm_;
  void*              plugin_handle_ = nullptr;
};

}  // namespace master
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_MASTER_SCHEDULER_H
