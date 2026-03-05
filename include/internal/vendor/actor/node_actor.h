// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// MMNode Actor implementation based on Protoactor-cpp

#ifndef AGENTFLOW_ACTOR_NODE_ACTOR_H
#define AGENTFLOW_ACTOR_NODE_ACTOR_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <memory>
#include <string>

#include "vendor/actor/actors.h"
#include "agent/common.h"
#include "protoactor/actor.h"

namespace protoactor {
class Context;
}

namespace agentflow {
namespace actor {

/// @brief MMNode Actor for executing individual workflow nodes
class NodeActor : public protoactor::Actor, public AgentFlowActor {
public:
  /// @brief Constructor
  /// @param node_id The ID of the node this actor represents
  /// @param node_config Configuration for the node
  NodeActor(const NodeId& node_id, const NodeConfig& node_config);

  /// @brief Destructor
  ~NodeActor() override = default;

  /// @brief Main message processing method (Protoactor interface)
  /// @param context The actor context
  void Receive(std::shared_ptr<protoactor::Context> context) override;

  /// @brief Process AgentFlow messages
  /// @param message_type Type of message
  /// @param message The message payload
  void process_message(const std::string& message_type, std::shared_ptr<void> message) override;

  // Getters
  const NodeId&     get_node_id() const { return node_id_; }
  const NodeConfig& get_config() const { return config_; }
  NodeState         get_state() const { return state_; }

private:
  NodeId     node_id_;
  NodeConfig config_;
  NodeState  state_ = NodeState::WAITING;
  DataMap    input_data_;
  DataMap    output_data_;
  int        retry_count_ = 0;
  uint64_t   start_time_;

  /// @brief Handle InitializeNodeMessage
  void handle_initialize_node(const InitializeNodeMessage& msg);

  /// @brief Handle ExecuteNodeMessage
  void handle_execute_node(const ExecuteNodeMessage& msg);

  /// @brief Handle StopNodeMessage
  void handle_stop_node(const StopNodeMessage& msg);

  /// @brief Handle NodeStatusMessage
  void handle_status_query(const NodeStatusMessage&             msg,
                           std::shared_ptr<protoactor::Context> context);

  /// @brief Handle NodeHealthCheckMessage
  void handle_health_check(const NodeHealthCheckMessage&        msg,
                           std::shared_ptr<protoactor::Context> context);

  /// @brief Send result back to parent workflow actor
  void send_result(Status status, const std::string& error_message = "");

  /// @brief Execute C++ Actor-based node
  bool execute_cpp_actor();

  /// @brief Execute shell script node
  bool execute_shell_script();

  /// @brief Execute Python script node
  bool execute_python_script();

  /// @brief Convert NodeState to string
  static std::string node_state_to_string(NodeState state);
};

}  // namespace actor
}  // namespace agentflow

#else

// Stub for when Protoactor is not available
#include "vendor/actor/actors.h"
#include "agent/common.h"

namespace agentflow {
namespace actor {

class NodeActor : public AgentFlowActor {
public:
  NodeActor(const NodeId& node_id, const NodeConfig& node_config) {
    (void)node_id;
    (void)node_config;
  }
  ~NodeActor() override = default;

  void process_message(const std::string& message_type, std::shared_ptr<void> message) override {
    (void)message_type;
    (void)message;
  }
};

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_NODE_ACTOR_H
