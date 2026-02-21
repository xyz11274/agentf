#include "runtime/master/scheduler.h"

namespace agentflow {
namespace runtime {
namespace master {

TaskScheduleResult Scheduler::schedule_task(
    const TaskScheduleRequest& request,
    const std::vector<WorkflowInstanceInfo>& workflow_instances) const {
  TaskScheduleResult result;

  const WorkflowInstanceInfo* best_same_instance = nullptr;
  const WorkflowInstanceInfo* best_same_object = nullptr;
  const WorkflowInstanceInfo* best_any = nullptr;

  for(const auto& wi : workflow_instances) {
    if(wi.workflow_id != request.workflow_id) {
      continue;
    }
    if(!best_any || wi.ref_count < best_any->ref_count) {
      best_any = &wi;
    }
    if(wi.agent_object_id == request.agent_object_id) {
      if(!best_same_object || wi.ref_count < best_same_object->ref_count) {
        best_same_object = &wi;
      }
      if(wi.agent_instance_id == request.agent_instance_id
         && (!best_same_instance || wi.ref_count < best_same_instance->ref_count)) {
        best_same_instance = &wi;
      }
    }
  }

  const WorkflowInstanceInfo* chosen = best_same_instance ? best_same_instance :
                                       (best_same_object ? best_same_object : best_any);
  if(!chosen) {
    return result;
  }

  result.scheduled = true;
  result.workflow_instance_id = chosen->workflow_instance_id;
  result.worker_id = chosen->worker_id;
  return result;
}

}  // namespace master
}  // namespace runtime
}  // namespace agentflow
