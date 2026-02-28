#include "agent/coordinator.h"

#include <sstream>

#include "agent/dataref_bridge.h"
#include "agent/session.h"
#include "agent/task.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace framework {

AgentCoordinator::AgentCoordinator(const Config& config,
                                   const std::shared_ptr<AgentTask>& coordinator_task)
    : config_(config), coordinator_task_(coordinator_task) {
}

std::string AgentCoordinator::resolve_task_workflow(
    const std::string& task_type, const std::vector<std::shared_ptr<Workflow>>& workflows) const {
  if(task_type != "task0") {
    return "";
  }
  std::string target = config_.coordinator_workflow;
  if(target.empty()) {
    const auto it = config_.task_workflows.find("0");
    if(it != config_.task_workflows.end()) {
      target = it->second;
    }
  }
  if(target.empty()) {
    return "";
  }
  for(const auto& workflow : workflows) {
    if(workflow && workflow->get_id() == target) {
      return workflow->get_id();
    }
  }
  for(const auto& workflow : workflows) {
    if(workflow && workflow->get_name() == target) {
      return workflow->get_id();
    }
  }
  return "";
}

std::shared_ptr<AgentTask> AgentCoordinator::build_task_for_message(
    const std::string& session_id, const AgentMsgQue::Message& message,
    const std::vector<std::shared_ptr<Workflow>>& workflows, int64_t suffix_counter) const {
  AgentTask::Config task_config;
  std::ostringstream task_id;
  task_id << session_id << "_task_" << suffix_counter;
  task_config.task_id = task_id.str();

  const auto type_it = message.metadata.find("task_type");
  task_config.task_type =
      (type_it != message.metadata.end()) ? type_it->second : config_.default_task_type;
  task_config.session_id = session_id;

  const auto workflow_it = message.metadata.find("workflow_id");
  if(workflow_it != message.metadata.end()) {
    task_config.workflow_id = workflow_it->second;
  } else if(const auto task_workflow = resolve_task_workflow(task_config.task_type, workflows);
            !task_workflow.empty()) {
    task_config.workflow_id = task_workflow;
  } else if(!workflows.empty()) {
    task_config.workflow_id = workflows.front()->get_id();
  }

  if(task_config.task_type == "collaboration") {
    const auto so_it = message.metadata.find("collaboration_so");
    if(so_it != message.metadata.end()) {
      task_config.collaboration_so_path = so_it->second;
    }
  }

  auto task = std::make_shared<AgentTask>(task_config);
  task->set_parameter("message_id", message.message_id);
  task->set_parameter("message_sender", message.sender_id);
  std::string payload = message.content;
  if(payload.empty() && !message.data_ref.empty()) {
    dataref_bridge::resolve_payload(message.data_ref, payload);
  }
  task->set_parameter("message_content", payload);
  task->set_parameter("message_data_ref", message.data_ref);
  return task;
}

std::vector<std::shared_ptr<AgentTask>> AgentCoordinator::process_session_messages(
    AgentSession& session, const std::vector<std::shared_ptr<Workflow>>& workflows) {
  std::vector<std::shared_ptr<AgentTask>> tasks;
  AgentMsgQue::Message                    message;
  int64_t                                 counter = 0;
  while(session.dequeue_received_message(message)) {
    auto task = build_task_for_message(session.get_session_id(), message, workflows, ++counter);
    session.add_task(task);
    tasks.push_back(task);
  }
  return tasks;
}

}  // namespace framework
}  // namespace agentflow
