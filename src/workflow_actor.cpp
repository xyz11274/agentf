// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Workflow Actor implementation

#include "agentflow/actor/workflow_actor.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include "protoactor/context.h"
#include "protoactor/pid.h"
#include "agentflow/basic/logger.h"
#include "agentflow/actor/node_actor.h"

namespace agentflow {
namespace actor {

WorkflowActor::WorkflowActor(const WorkflowId& workflow_id,
                             const WorkflowConfig& workflow_config)
    : workflow_id_(workflow_id), config_(workflow_config) {
  state_ = WorkflowState::CREATED;
}

void WorkflowActor::Receive(std::shared_ptr<protoactor::Context> context) {
  if (!context) {
    return;
  }

  auto message = context->Message();
  if (!message) {
    return;
  }

  // Type-safe message dispatch would require RTTI or custom serialization
  // For now, we handle the message in process_message
}

void WorkflowActor::process_message(const std::string& message_type,
                                    std::shared_ptr<void> message) {
  if (message_type == "ExecuteWorkflowMessage") {
    // Handle workflow execution
    state_ = WorkflowState::RUNNING;
    
    // In a real implementation, would:
    // 1. Parse DAG structure
    // 2. Identify executable nodes (no dependencies)
    // 3. Spawn node actors
    // 4. Wait for results
    
    state_ = WorkflowState::COMPLETED;
    send_workflow_result(state_);
  } else if (message_type == "NodeResultMessage") {
    // Handle result from a child node actor
    // Update workflow state based on node results
  }
}

void WorkflowActor::handle_execute_workflow(const ExecuteWorkflowMessage& msg) {
  if (msg.workflow_id != workflow_id_) {
    return;
  }

  state_ = WorkflowState::RUNNING;

  // Parse workflow DAG and schedule node execution
  // For each node without dependencies, spawn a NodeActor
  // This is a simplified implementation
  
  Logger& logger = Logger::get_instance();
  logger.log(LogLevel::INFO,
             "Workflow [" + workflow_id_ + "] started execution");

  // In real implementation:
  // 1. Load workflow definition from file or DAG structure
  // 2. Start with nodes that have no dependencies
  // 3. As nodes complete, spawn dependent nodes
  // 4. When all nodes complete, mark workflow as complete

  state_ = WorkflowState::COMPLETED;
  send_workflow_result(state_);
}

void WorkflowActor::handle_node_result(const NodeResultMessage& msg) {
  if (msg.status == Status::SUCCESS) {
    // Store node output
    output_data_.insert(msg.output_data.begin(), msg.output_data.end());

    // Check if dependent nodes can be executed
    // Spawn dependent nodes if all their dependencies are ready
  } else {
    // Node failed
    state_ = WorkflowState::FAILED;
    send_workflow_result(state_);
  }
}

std::shared_ptr<protoactor::PID> WorkflowActor::spawn_node_actor(
    const NodeId& node_id) {
  // This would create and spawn a NodeActor using the actor system
  // Implementation would look like:
  // auto props = protoactor::Props::FromProducer(
  //     [node_id]() { return std::make_shared<NodeActor>(node_id, ...); }
  // );
  // auto pid = context->Spawn(props);
  
  return nullptr;  // Placeholder
}

void WorkflowActor::send_workflow_result(WorkflowState state) {
  Logger& logger = Logger::get_instance();
  logger.log(LogLevel::INFO,
             "Workflow [" + workflow_id_ + "] finished with state: " +
             std::to_string(static_cast<int>(state)));
}

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
