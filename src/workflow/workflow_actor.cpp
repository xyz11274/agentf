// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Workflow Actor implementation

#include "workflow/workflow_actor.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <algorithm>

#include "workflow/node_actor.h"
#include "utils/logger.h"
#include "protoactor/context.h"
#include "protoactor/pid.h"
#include "protoactor/props.h"

namespace agentflow {
namespace actor {

WorkflowActor::WorkflowActor(const WorkflowId& workflow_id, const WorkflowConfig& workflow_config)
    : workflow_id_(workflow_id), config_(workflow_config) {
  state_ = WorkflowState::CREATED;
}

void WorkflowActor::Receive(std::shared_ptr<protoactor::Context> context) {
  if(!context) {
    return;
  }

  auto message = context->Message();
  if(!message) {
    return;
  }

  // Dispatch based on message type
  auto message_type = message->GetType();

  if(message_type == "ExecuteWorkflowMessage") {
    auto msg = std::static_pointer_cast<ExecuteWorkflowMessage>(message->GetPayload());
    if(msg) {
      handle_execute_workflow(*msg);
    }
  } else if(message_type == "NodeResultMessage") {
    auto msg = std::static_pointer_cast<NodeResultMessage>(message->GetPayload());
    if(msg) {
      handle_node_result(*msg);
    }
  } else if(message_type == "WorkflowStatusMessage") {
    // Handle status query
    handle_status_query(context);
  }
}

void WorkflowActor::process_message(const std::string&    message_type,
                                    std::shared_ptr<void> message) {
  // This method is called through AgentFlowActor interface
  // In Protoactor implementation, we use Receive() directly
  // This is a compatibility method for non-Protoactor paths

  if(message_type == "ExecuteWorkflowMessage") {
    auto msg = std::static_pointer_cast<ExecuteWorkflowMessage>(message);
    if(msg) {
      handle_execute_workflow(*msg);
    }
  } else if(message_type == "NodeResultMessage") {
    auto msg = std::static_pointer_cast<NodeResultMessage>(message);
    if(msg) {
      handle_node_result(*msg);
    }
  }
}

void WorkflowActor::handle_execute_workflow(const ExecuteWorkflowMessage& msg) {
  if(msg.workflow_id != workflow_id_) {
    return;
  }

  if(state_ == WorkflowState::RUNNING) {
    Logger::instance().warn("Workflow [" + workflow_id_ + "] already running");
    return;
  }

  state_      = WorkflowState::RUNNING;
  input_data_ = msg.input_data;
  completed_nodes_.clear();
  failed_nodes_.clear();

  Logger::instance().info("Workflow [" + workflow_id_ + "] started execution");

  // Parse workflow graph and schedule node execution
  // For each node without dependencies, spawn a NodeActor
  schedule_executable_nodes();

  // Check if workflow is complete immediately (empty workflow)
  check_workflow_completion();
}

void WorkflowActor::handle_node_result(const NodeResultMessage& msg) {
  // Update workflow state based on node results

  if(msg.status == Status::SUCCESS) {
    Logger::instance().info("Node [" + msg.node_id + "] completed successfully");

    // Store node output
    for(const auto& [key, value] : msg.output_data) {
      output_data_[key] = value;
    }

    // Mark node as completed
    completed_nodes_.insert(msg.node_id);

    // Check if dependent nodes can be executed
    schedule_executable_nodes();

  } else {
    Logger::instance().error("Node [" + msg.node_id + "] failed: " + msg.error_message);

    // Mark node as failed
    failed_nodes_.insert(msg.node_id);

    // Check workflow error handling strategy
    if(config_.error_handling == ErrorHandlingStrategy::STOP_ON_ERROR) {
      state_ = WorkflowState::FAILED;
      send_workflow_result(state_);
      return;
    }
    // Continue execution for other strategies
  }

  // Check if workflow is complete
  check_workflow_completion();
}

void WorkflowActor::handle_status_query(std::shared_ptr<protoactor::Context> context) {
  // Send workflow status back to sender
  DataMap metadata;
  metadata["state"]           = workflow_state_to_string(state_);
  metadata["completed_nodes"] = static_cast<int64_t>(completed_nodes_.size());
  metadata["failed_nodes"]    = static_cast<int64_t>(failed_nodes_.size());
  metadata["active_nodes"]    = static_cast<int64_t>(node_actors_.size());

  // Send response using Protoactor's Reply
  if(context && context->Sender()) {
    auto status_msg =
        std::make_shared<WorkflowStatusMessage>(workflow_id_, state_, output_data_, metadata);
    context->Reply(status_msg);
  }
}

void WorkflowActor::schedule_executable_nodes() {
  if(state_ != WorkflowState::RUNNING) {
    return;
  }

  // For each node in the workflow, check if it can be executed
  // A node can be executed if:
  // 1. It hasn't been completed yet
  // 2. It hasn't been started yet
  // 3. All its dependencies are satisfied

  // In a real implementation, we would parse the workflow definition
  // For now, we assume the config contains the node list

  for(const auto& node_config : config_.nodes) {
    const NodeId& node_id = node_config.id;

    // Skip if already completed or failed
    if(completed_nodes_.find(node_id) != completed_nodes_.end()
       || failed_nodes_.find(node_id) != failed_nodes_.end()) {
      continue;
    }

    // Skip if already running (has an actor)
    if(node_actors_.find(node_id) != node_actors_.end()) {
      continue;
    }

    // Check if all dependencies are satisfied
    bool dependencies_satisfied = true;
    for(const auto& dependency : node_config.dependencies) {
      if(completed_nodes_.find(dependency) == completed_nodes_.end()) {
        dependencies_satisfied = false;
        break;
      }
    }

    if(dependencies_satisfied) {
      // Spawn node actor
      auto node_pid = spawn_node_actor(node_id);
      if(node_pid) {
        node_actors_[node_id] = node_pid;

        // Send execute message to node actor
        ExecuteNodeMessage exec_msg(node_id, input_data_);
        auto               exec_ptr = std::make_shared<ExecuteNodeMessage>(exec_msg);

        // Send using Protoactor's messaging system
        // In a real implementation, we'd use context->Send()
        Logger::instance().info("Spawning node actor for [" + node_id + "]");
      }
    }
  }
}

std::shared_ptr<protoactor::PID> WorkflowActor::spawn_node_actor(const NodeId& node_id) {
  // Find node configuration
  NodeConfig node_config;
  bool       found = false;
  for(const auto& config : config_.nodes) {
    if(config.id == node_id) {
      node_config = config;
      found       = true;
      break;
    }
  }

  if(!found) {
    Logger::instance().error("Node [" + node_id + "] not found in workflow config");
    return nullptr;
  }

  try {
    // Create NodeActor using Protoactor's props
    auto props = protoactor::Props::FromProducer(
        [node_id, node_config]() -> std::shared_ptr<protoactor::Actor> {
          return std::make_shared<NodeActor>(node_id, node_config);
        });

    // Apply mailbox configuration if specified
    if(node_config.mailbox_size > 0) {
      props = props.WithMailbox(node_config.mailbox_size);
    }

    // Spawn using the actor system
    // In a real implementation, we'd get the root and spawn
    // For now, return nullptr as placeholder
    Logger::instance().info("Creating NodeActor for [" + node_id + "]");

    return nullptr;  // Placeholder - would return actual PID

  } catch(const std::exception& e) {
    Logger::instance().error("Failed to spawn node actor [" + node_id
                             + "]: " + std::string(e.what()));
    return nullptr;
  }
}

void WorkflowActor::check_workflow_completion() {
  // Check if all nodes have completed or failed
  size_t total_nodes = config_.nodes.size();
  size_t completed   = completed_nodes_.size();
  size_t failed      = failed_nodes_.size();

  if(completed + failed >= total_nodes) {
    if(failed > 0) {
      state_ = WorkflowState::FAILED;
    } else {
      state_ = WorkflowState::COMPLETED;
    }

    Logger::instance().info("Workflow [" + workflow_id_
                            + "] finished: " + workflow_state_to_string(state_));
    send_workflow_result(state_);
  }
}

void WorkflowActor::send_workflow_result(WorkflowState state) {
  // Cleanup node actors
  for(auto& [node_id, pid] : node_actors_) {
    if(pid && !pid->Stopped()) {
      // Stop the node actor
      // In a real implementation, we'd use root->Stop(pid)
      Logger::instance().debug("Stopping node actor [" + node_id + "]");
    }
  }
  node_actors_.clear();

  // Send result to parent
  // This would be done through Protoactor's messaging system
  Logger::instance().info("Workflow result sent: " + workflow_state_to_string(state));
}

std::string WorkflowActor::workflow_state_to_string(WorkflowState state) {
  switch(state) {
    case WorkflowState::CREATED:
      return "CREATED";
    case WorkflowState::RUNNING:
      return "RUNNING";
    case WorkflowState::COMPLETED:
      return "COMPLETED";
    case WorkflowState::FAILED:
      return "FAILED";
    case WorkflowState::STOPPED:
      return "STOPPED";
    default:
      return "UNKNOWN";
  }
}

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
