// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Node Actor implementation

#include "agentflow/actor/node_actor.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <chrono>

#include "agentflow/basic/logger.h"
#include "protoactor/context.h"
#include "protoactor/pid.h"

namespace agentflow {
namespace actor {

NodeActor::NodeActor(const NodeId& node_id, const NodeConfig& node_config)
    : node_id_(node_id), config_(node_config), start_time_(0) {
  state_ = NodeState::WAITING;
}

void NodeActor::Receive(std::shared_ptr<protoactor::Context> context) {
  if(!context) {
    return;
  }

  // Check for special messages from Protoactor system
  // Protoactor-cpp sends Started/Stopping messages via Context

  auto message = context->Message();
  if(!message) {
    return;
  }

  // Dispatch based on message type
  // Using Protoactor's message type system
  auto message_type = message->GetType();

  if(message_type == "ExecuteNodeMessage") {
    auto msg = std::static_pointer_cast<ExecuteNodeMessage>(message->GetPayload());
    if(msg) {
      handle_execute_node(*msg);
    }
  } else if(message_type == "InitializeNodeMessage") {
    auto msg = std::static_pointer_cast<InitializeNodeMessage>(message->GetPayload());
    if(msg) {
      handle_initialize_node(*msg);
    }
  } else if(message_type == "StopNodeMessage") {
    auto msg = std::static_pointer_cast<StopNodeMessage>(message->GetPayload());
    if(msg) {
      handle_stop_node(*msg);
    }
  } else if(message_type == "NodeStatusMessage") {
    auto msg = std::static_pointer_cast<NodeStatusMessage>(message->GetPayload());
    if(msg) {
      handle_status_query(*msg, context);
    }
  } else if(message_type == "NodeHealthCheckMessage") {
    auto msg = std::static_pointer_cast<NodeHealthCheckMessage>(message->GetPayload());
    if(msg) {
      handle_health_check(*msg, context);
    }
  }
}

void NodeActor::process_message(const std::string& message_type, std::shared_ptr<void> message) {
  // This method is called through the AgentFlowActor interface
  // In the Protoactor implementation, we use Receive() directly
  // This is a compatibility method for non-Protoactor paths

  if(message_type == "ExecuteNodeMessage") {
    auto msg = std::static_pointer_cast<ExecuteNodeMessage>(message);
    if(msg) {
      handle_execute_node(*msg);
    }
  } else if(message_type == "InitializeNodeMessage") {
    auto msg = std::static_pointer_cast<InitializeNodeMessage>(message);
    if(msg) {
      handle_initialize_node(*msg);
    }
  } else if(message_type == "StopNodeMessage") {
    auto msg = std::static_pointer_cast<StopNodeMessage>(message);
    if(msg) {
      handle_stop_node(*msg);
    }
  }
}

void NodeActor::handle_initialize_node(const InitializeNodeMessage& msg) {
  if(msg.node_id != node_id_) {
    return;
  }

  config_      = msg.config;
  state_       = NodeState::READY;
  retry_count_ = 0;

  Logger::instance().info("Node [" + node_id_ + "] initialized");
}

void NodeActor::handle_execute_node(const ExecuteNodeMessage& msg) {
  if(msg.node_id != node_id_) {
    return;
  }

  if(state_ == NodeState::FAILED && retry_count_ >= config_.max_retries) {
    Logger::instance().error("Node [" + node_id_ + "] exceeded max retries");
    send_result(Status::FAILED, "Max retries exceeded");
    return;
  }

  input_data_ = msg.input_data;
  state_      = NodeState::EXECUTING;
  start_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();

  Logger::instance().info("Node [" + node_id_ + "] executing");

  // Execute node logic - this would call the actual node implementation
  // For now, simulate successful execution
  bool        success = true;
  std::string error_msg;

  // Simulate execution based on execution mode
  if(config_.execution_mode == ExecutionMode::CPP_ACTOR) {
    // C++ Actor-based execution
    success = execute_cpp_actor();
  } else if(config_.execution_mode == ExecutionMode::SHELL_SCRIPT) {
    // Shell script execution
    success = execute_shell_script();
  } else if(config_.execution_mode == ExecutionMode::PYTHON_SCRIPT) {
    // Python script execution
    success = execute_python_script();
  } else {
    // Default to success
    success = true;
  }

  if(success) {
    state_ = NodeState::SUCCESS;
    send_result(Status::SUCCESS, "");
  } else {
    state_ = NodeState::FAILED;
    retry_count_++;
    send_result(Status::FAILED, error_msg.empty() ? "Execution failed" : error_msg);
  }
}

void NodeActor::handle_stop_node(const StopNodeMessage& msg) {
  if(msg.node_id != node_id_) {
    return;
  }

  state_ = msg.force ? NodeState::TERMINATED : NodeState::STOPPED;

  Logger::instance().info("Node [" + node_id_
                          + "] stopped (force: " + std::string(msg.force ? "true" : "false") + ")");
}

void NodeActor::handle_status_query(const NodeStatusMessage&             msg,
                                    std::shared_ptr<protoactor::Context> context) {
  if(msg.node_id != node_id_) {
    return;
  }

  // Send status response back to sender
  uint64_t uptime = 0;
  if(start_time_ > 0) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
    uptime = now - start_time_;
  }

  DataMap metadata;
  metadata["retry_count"]    = static_cast<int64_t>(retry_count_);
  metadata["uptime_ms"]      = static_cast<int64_t>(uptime);
  metadata["execution_mode"] = static_cast<int64_t>(config_.execution_mode);

  NodeStatusResponse response(node_id_, state_, metadata, msg.query_id);

  // Use Protoactor's Reply to send response
  if(context && context->Sender()) {
    // Send response back to sender
    auto response_ptr = std::make_shared<NodeStatusResponse>(response);
    context->Reply(response_ptr);
  }
}

void NodeActor::handle_health_check(const NodeHealthCheckMessage&        msg,
                                    std::shared_ptr<protoactor::Context> context) {
  if(msg.node_id != node_id_) {
    return;
  }

  bool        healthy = (state_ == NodeState::READY || state_ == NodeState::SUCCESS);
  std::string status  = node_state_to_string(state_);

  uint64_t uptime = 0;
  if(start_time_ > 0) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
    uptime = now - start_time_;
  }

  NodeHealthCheckResponse response(node_id_, healthy, status, uptime);

  // Send response back to sender
  if(context && context->Sender()) {
    auto response_ptr = std::make_shared<NodeHealthCheckResponse>(response);
    context->Reply(response_ptr);
  }
}

void NodeActor::send_result(Status status, const std::string& error_message) {
  NodeResultMessage result(node_id_, status, output_data_, "", error_message);

  Logger& logger = Logger::instance();
  if(status == Status::SUCCESS) {
    logger.info("Node [" + node_id_ + "] completed successfully");
  } else {
    logger.error("Node [" + node_id_ + "] failed: " + error_message);
  }

  // The result would be sent to the parent WorkflowActor
  // This is done through Protoactor's messaging system
  // The parent is tracked by the WorkflowActor that spawned this NodeActor
}

bool NodeActor::execute_cpp_actor() {
  // Placeholder for C++ Actor-based execution
  // In real implementation, this would execute the node's logic
  // using the Actor framework

  // Simulate execution time
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Set some output data
  output_data_["result"]         = static_cast<int64_t>(1);
  output_data_["execution_mode"] = std::string("cpp_actor");

  return true;
}

bool NodeActor::execute_shell_script() {
  // Placeholder for shell script execution
  Logger::instance().warn("Shell script execution not yet implemented");

  output_data_["result"]         = static_cast<int64_t>(1);
  output_data_["execution_mode"] = std::string("shell_script");

  return true;
}

bool NodeActor::execute_python_script() {
  // Placeholder for Python script execution
  Logger::instance().warn("Python script execution not yet implemented");

  output_data_["result"]         = static_cast<int64_t>(1);
  output_data_["execution_mode"] = std::string("python_script");

  return true;
}

std::string NodeActor::node_state_to_string(NodeState state) {
  switch(state) {
    case NodeState::WAITING:
      return "WAITING";
    case NodeState::READY:
      return "READY";
    case NodeState::EXECUTING:
      return "EXECUTING";
    case NodeState::SUCCESS:
      return "SUCCESS";
    case NodeState::FAILED:
      return "FAILED";
    case NodeState::STOPPED:
      return "STOPPED";
    case NodeState::TERMINATED:
      return "TERMINATED";
    default:
      return "UNKNOWN";
  }
}

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
