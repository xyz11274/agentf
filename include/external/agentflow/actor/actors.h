// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Actor-based implementation for AgentFlow
// This file integrates Protoactor-cpp framework with AgentFlow

#ifndef AGENTFLOW_ACTOR_ACTORS_H
#define AGENTFLOW_ACTOR_ACTORS_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "agentflow/common.h"

// Forward declarations - Protoactor types
namespace protoactor {
class Actor;
class Context;
class PID;
class ActorSystem;
}  // namespace protoactor

namespace agentflow {
namespace actor {

// Forward declarations
class NodeActor;
class WorkflowActor;
class AgentActor;

// Message types used in the actor system

/// @brief Message for executing a node
struct ExecuteNodeMessage {
  NodeId      node_id;
  DataMap     input_data;
  std::string correlation_id;

  ExecuteNodeMessage(const NodeId& id, const DataMap& data, const std::string& corr_id = "")
      : node_id(id), input_data(data), correlation_id(corr_id) {}
};

/// @brief Message containing node execution result
struct NodeResultMessage {
  NodeId      node_id;
  Status      status;
  DataMap     output_data;
  std::string correlation_id;
  std::string error_message;

  NodeResultMessage(const NodeId& id, Status s, const DataMap& data,
                    const std::string& corr_id = "", const std::string& error = "")
      : node_id(id), status(s), output_data(data), correlation_id(corr_id), error_message(error) {}
};

/// @brief Message for initializing a node
struct InitializeNodeMessage {
  NodeId     node_id;
  NodeConfig config;

  InitializeNodeMessage(const NodeId& id, const NodeConfig& cfg) : node_id(id), config(cfg) {}
};

/// @brief Message for stopping a node
struct StopNodeMessage {
  NodeId node_id;
  bool   force;

  StopNodeMessage(const NodeId& id, bool f = false) : node_id(id), force(f) {}
};

/// @brief Message for querying node status
struct NodeStatusMessage {
  NodeId      node_id;
  std::string query_id;

  NodeStatusMessage(const NodeId& id, const std::string& qid = "") : node_id(id), query_id(qid) {}
};

/// @brief Message containing node status
struct NodeStatusResponse {
  NodeId      node_id;
  NodeState   state;
  DataMap     metadata;
  std::string query_id;

  NodeStatusResponse(const NodeId& id, NodeState s, const DataMap& meta,
                     const std::string& qid = "")
      : node_id(id), state(s), metadata(meta), query_id(qid) {}
};

/// @brief Message for node health check
struct NodeHealthCheckMessage {
  NodeId node_id;

  explicit NodeHealthCheckMessage(const NodeId& id) : node_id(id) {}
};

/// @brief Message containing health check result
struct NodeHealthCheckResponse {
  NodeId      node_id;
  bool        healthy;
  std::string health_status;
  uint64_t    uptime_ms;

  NodeHealthCheckResponse(const NodeId& id, bool h, const std::string& status, uint64_t uptime = 0)
      : node_id(id), healthy(h), health_status(status), uptime_ms(uptime) {}
};

/// @brief Message for workflow execution
struct ExecuteWorkflowMessage {
  WorkflowId workflow_id;
  DataMap    input_data;

  ExecuteWorkflowMessage(const WorkflowId& id, const DataMap& data)
      : workflow_id(id), input_data(data) {}
};

/// @brief Message containing workflow execution result
struct WorkflowResultMessage {
  WorkflowId    workflow_id;
  WorkflowState state;
  DataMap       output_data;

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
  virtual void process_message(const std::string& message_type, std::shared_ptr<void> message) = 0;
};

}  // namespace actor
}  // namespace agentflow

#else

// Stub implementation when Protoactor-cpp is not available
#include <memory>
#include <string>

#include "agentflow/common.h"

namespace agentflow {
namespace actor {

struct ExecuteNodeMessage {
  NodeId  node_id;
  DataMap input_data;
};

struct NodeResultMessage {
  NodeId  node_id;
  Status  status;
  DataMap output_data;
};

struct ExecuteWorkflowMessage {
  WorkflowId workflow_id;
  DataMap    input_data;
};

struct WorkflowResultMessage {
  WorkflowId    workflow_id;
  WorkflowState state;
  DataMap       output_data;
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

  virtual void process_message(const std::string& message_type, std::shared_ptr<void> message) {
    (void)message_type;
    (void)message;
  }
};

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_ACTORS_H
