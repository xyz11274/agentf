#include "agent/coordinator.h"

#include <sstream>

#include "agent/agent.h"
#include "agent/dataref_bridge.h"
#include "agent/session.h"
#include "agent/task.h"
#include "workflow/workflow.h"
#include <filesystem>

namespace agentflow {
namespace framework {

SessionTaskCoordinator::SessionTaskCoordinator(const Config& config,
                                               const std::shared_ptr<MMAgentTask>& coordinator_task)
    : config_(config), coordinator_task_(coordinator_task) {
}

std::string SessionTaskCoordinator::resolve_task_workflow(
    const std::string& task_type, const std::vector<std::shared_ptr<MMWorkflow>>& workflows) const {
  std::string target;
  if(task_type == "dispatcher") {
    target = config_.dispatcher_workflow;
    if(target.empty()) {
      const auto dispatcher_it = config_.task_workflows.find("dispatcher");
      if(dispatcher_it != config_.task_workflows.end()) {
        target = dispatcher_it->second;
      } else {
        target = config_.coordinator_workflow;
      }
    }
  } else if(task_type == "coordinator") {
    target = config_.coordinator_workflow;
    if(target.empty()) {
      const auto coordinator_it = config_.task_workflows.find("coordinator");
      if(coordinator_it != config_.task_workflows.end()) {
        target = coordinator_it->second;
      }
    }
  } else {
    const auto workflow_it = config_.task_workflows.find(task_type);
    if(workflow_it != config_.task_workflows.end()) {
      target = workflow_it->second;
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
  const std::filesystem::path target_path(target);
  const std::string target_filename = target_path.filename().string();
  for(const auto& workflow : workflows) {
    if(workflow) {
      const std::filesystem::path dsl_path(workflow->get_config().dsl_file);
      if(dsl_path == target_path || dsl_path.filename().string() == target_filename) {
        return workflow->get_id();
      }
    }
  }
  return "";
}

std::shared_ptr<MMAgentTask> SessionTaskCoordinator::build_task_for_message(
    const std::string& session_id, const MMAgentMsgQue::Message& message,
    const std::vector<std::shared_ptr<MMWorkflow>>& workflows, int64_t suffix_counter) const {
  MMAgentTask::Config task_config;
  std::ostringstream task_id;
  task_id << session_id << "_task_" << suffix_counter;
  task_config.task_index = static_cast<int>(suffix_counter);
  task_config.task_id = task_id.str();

  const auto type_it = message.metadata.find("task_type");
  task_config.task_type =
      (type_it != message.metadata.end()) ? type_it->second : config_.default_task_type;
  task_config.session_id = session_id;
  task_config.task_context_workflow = config_.task_context_workflow;

  const auto workflow_it = message.metadata.find("workflow_id");
  if(workflow_it != message.metadata.end()) {
    task_config.workflow_id = workflow_it->second;
  } else if(const auto task_workflow = resolve_task_workflow(task_config.task_type, workflows);
            !task_workflow.empty()) {
    task_config.workflow_id = task_workflow;
  } else if(!workflows.empty()) {
    task_config.workflow_id = workflows.front()->get_id();
  }

  const auto workflow_file_it = message.metadata.find("workflow_file");
  if(workflow_file_it != message.metadata.end()) {
    task_config.workflow_file = workflow_file_it->second;
  } else {
    for(const auto& workflow : workflows) {
      if(workflow && workflow->get_id() == task_config.workflow_id) {
        task_config.workflow_file = workflow->get_config().dsl_file;
        break;
      }
    }
  }

  if(task_config.task_type == "collaboration") {
    const auto so_it = message.metadata.find("collaboration_so");
    if(so_it != message.metadata.end()) {
      task_config.collaboration_so_path = so_it->second;
    }
  }
  if(task_config.task_type == "skill") {
    const auto skill_name_it = message.metadata.find("skill_name");
    if(skill_name_it != message.metadata.end()) {
      task_config.skill_name = skill_name_it->second;
    }
    const auto skill_dir_it = message.metadata.find("skill_config_dir");
    if(skill_dir_it != message.metadata.end()) {
      task_config.skill_config_dir = skill_dir_it->second;
    }
  }

  auto task = std::make_shared<MMAgentTask>(task_config);
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

std::vector<std::shared_ptr<MMAgentTask>> SessionTaskCoordinator::process_session_messages(
    MMAgentSession& session, const std::vector<std::shared_ptr<MMWorkflow>>& workflows) {
  std::vector<std::shared_ptr<MMAgentTask>> tasks;
  MMAgentMsgQue::Message                    message;
  int64_t                                 counter = static_cast<int64_t>(session.get_tasks().size());
  while(config_.input_queue_type == InputQueueType::INTENT_REQUEST
            ? session.dequeue_intent_request_message(message)
            : session.dequeue_output_result_message(message)) {
    auto task = build_task_for_message(session.get_session_id(), message, workflows, ++counter);
    session.add_task(task);
    tasks.push_back(task);
  }
  return tasks;
}

DataMap SessionTaskCoordinator::aggregate_session_results(
    MMAgent& agent, const std::vector<std::string>& output_keys) const {
  DataMap result;
  auto session_ids = agent.get_all_session_ids();

  for(const auto& session_id : session_ids) {
    for(const auto& key : output_keys) {
      DataVariant value;
      if(agent.get_session_output(session_id, key, value)) {
        // Store as "session:<session_id>:<key>"
        std::string result_key = "session:" + session_id + ":" + key;
        result[result_key] = value;
      }
    }
  }
  return result;
}

}  // namespace framework
}  // namespace agentflow
