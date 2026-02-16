// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Node Actor implementation

#include "agentflow/actor/node_actor.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include "protoactor/context.h"
#include "agentflow/basic/logger.h"

namespace agentflow {
namespace actor {

NodeActor::NodeActor(const NodeId& node_id, const NodeConfig& node_config)
    : node_id_(node_id), config_(node_config) {
  state_ = NodeState::WAITING;
}

void NodeActor::Receive(std::shared_ptr<protoactor::Context> context) {
  if (!context) {
    return;
  }

  auto message = context->Message();
  if (!message) {
    return;
  }

  // Type-safe message dispatch would require RTTI or custom serialization
  // For now, we handle the message in process_message
  // In a real implementation, you'd use a message router or visitor pattern

  // Placeholder: would need actual message type checking
  // and casting based on Protoactor's message system
}

void NodeActor::process_message(const std::string& message_type,
                                std::shared_ptr<void> message) {
  if (message_type == "ExecuteNodeMessage") {
    // Cast and handle execute message
    // This is a simplified implementation
    state_ = NodeState::EXECUTING;
    
    // Simulate node execution
    // In real scenario, would call actual node execution logic
    
    // Transition to success
    state_ = NodeState::SUCCESS;
    
    // Send result back
    send_result(Status::SUCCESS);
  }
}

void NodeActor::handle_execute_node(const ExecuteNodeMessage& msg) {
  if (msg.node_id != node_id_) {
    return;
  }

  input_data_ = msg.input_data;
  state_ = NodeState::EXECUTING;

  // Execute node logic
  // This would be the actual node execution implementation
  // For now, we just transition to success

  state_ = NodeState::SUCCESS;
  send_result(Status::SUCCESS);
}

void NodeActor::send_result(Status status) {
  // Send NodeResultMessage back to parent actor
  // This would be implemented using Protoactor's messaging system
  
  Logger& logger = Logger::get_instance();
  if (status == Status::SUCCESS) {
    logger.log(LogLevel::INFO,
               "Node [" + node_id_ + "] completed successfully");
  } else {
    logger.log(LogLevel::ERROR,
               "Node [" + node_id_ + "] failed with status: " +
               std::to_string(static_cast<int>(status)));
  }
}

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
