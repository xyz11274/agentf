// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Actor-based implementation for AgentFlow
// This file integrates Protoactor-cpp framework with AgentFlow

#ifndef AGENTFLOW_ACTOR_ACTORS_H
#define AGENTFLOW_ACTOR_ACTORS_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <memory>
#include <string>
#include <map>
#include <functional>
#include "agentflow/common.h"

// Forward declarations - Protoactor types
namespace protoactor {
class Actor;
class Context;
class PID;
class ActorSystem;
}

namespace agentflow {
namespace actor {

// Forward declarations
class NodeActor;
class WorkflowActor;
class AgentActor;

// Message types used in the actor system

/// @brief Message for executing a node
struct ExecuteNodeMessage {
  NodeId node_id;
  DataMap input_data;
  
  ExecuteNodeMessage(const NodeId& id, const DataMap& data)
      : node_id(id), input_data(data) {}
};

/// @brief Message containing node execution result
struct NodeResultMessage {
  NodeId node_id;
  Status status;
  DataMap output_data;
  
  NodeResultMessage(const NodeId& id, Status s, const DataMap& data)
      : node_id(id), status(s), output_data(data) {}
};

/// @brief Message for workflow execution
struct ExecuteWorkflowMessage {
  WorkflowId workflow_id;
  DataMap input_data;
  
  ExecuteWorkflowMessage(const WorkflowId& id, const DataMap& data)
      : workflow_id(id), input_data(data) {}
};

/// @brief Message containing workflow execution result
struct WorkflowResultMessage {
  WorkflowId workflow_id;
  WorkflowState state;
  DataMap output_data;
  
  WorkflowResultMessage(const WorkflowId& id, WorkflowState s, const DataMap& data)
      : workflow_id(id), state(s), output_data(data) {}
};

/// @brief Message for agent operations
struct AgentOperationMessage {
  enum class Operation {
    INITIALIZE,
    START,
    STOP,
    PAUSE,
    RESUME,
  };
  
  Operation operation;
  
  explicit AgentOperationMessage(Operation op) : operation(op) {}
};

/// @brief Base class for AgentFlow actors
class AgentFlowActor {
 public:
  virtual ~AgentFlowActor() = default;
  
  /// @brief Process a message
  virtual void process_message(const std::string& message_type,
                               std::shared_ptr<void> message) = 0;
};

}  // namespace actor
}  // namespace agentflow

#else

// Stub implementation when Protoactor-cpp is not available
namespace agentflow {
namespace actor {

struct ExecuteNodeMessage {
  NodeId node_id;
  DataMap input_data;
};

struct NodeResultMessage {
  NodeId node_id;
  Status status;
  DataMap output_data;
};

struct ExecuteWorkflowMessage {
  WorkflowId workflow_id;
  DataMap input_data;
};

struct WorkflowResultMessage {
  WorkflowId workflow_id;
  WorkflowState state;
  DataMap output_data;
};

struct AgentOperationMessage {
  enum class Operation {
    INITIALIZE,
    START,
    STOP,
    PAUSE,
    RESUME,
  };
  Operation operation;
};

class AgentFlowActor {
 public:
  virtual ~AgentFlowActor() = default;
  
  virtual void process_message(const std::string& message_type,
                               std::shared_ptr<void> message) {}
};

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_ACTORS_H
