#ifndef AGENTFLOW_RUNTIME_MASTER_SCHEDULER_H
#define AGENTFLOW_RUNTIME_MASTER_SCHEDULER_H

#include <string>
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
  Scheduler() = default;
  ~Scheduler() = default;

  TaskScheduleResult schedule_task(const TaskScheduleRequest& request,
                                   const std::vector<WorkflowInstanceInfo>& workflow_instances) const;
};

}  // namespace master
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_MASTER_SCHEDULER_H
