// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Node Actor implementation based on Protoactor-cpp

#ifndef AGENTFLOW_ACTOR_NODE_ACTOR_H
#define AGENTFLOW_ACTOR_NODE_ACTOR_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <memory>
#include <string>
#include "agentflow/common.h"
#include "agentflow/actor/actors.h"
#include "protoactor/actor.h"

namespace agentflow {
namespace actor {

/// @brief Node Actor for executing individual workflow nodes
class NodeActor : public protoactor::Actor,
                   public AgentFlowActor {
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
  void process_message(const std::string& message_type,
                       std::shared_ptr<void> message) override;

  // Getters
  const NodeId& get_node_id() const { return node_id_; }
  const NodeConfig& get_config() const { return config_; }
  NodeState get_state() const { return state_; }

 private:
  NodeId node_id_;
  NodeConfig config_;
  NodeState state_ = NodeState::WAITING;
  DataMap input_data_;
  DataMap output_data_;
  int retry_count_ = 0;

  /// @brief Handle ExecuteNodeMessage
  void handle_execute_node(const ExecuteNodeMessage& msg);

  /// @brief Send result back to parent workflow actor
  void send_result(Status status);
};

}  // namespace actor
}  // namespace agentflow

#else

// Stub for when Protoactor is not available
#include "agentflow/common.h"
#include "agentflow/actor/actors.h"

namespace agentflow {
namespace actor {

class NodeActor : public AgentFlowActor {
 public:
  NodeActor(const NodeId& node_id, const NodeConfig& node_config) {}
  ~NodeActor() override = default;

  void process_message(const std::string& message_type,
                       std::shared_ptr<void> message) override {}
};

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_NODE_ACTOR_H
