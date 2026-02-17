// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Workflow Actor implementation

#ifndef AGENTFLOW_ACTOR_WORKFLOW_ACTOR_H
#define AGENTFLOW_ACTOR_WORKFLOW_ACTOR_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "agentflow/actor/actors.h"
#include "agentflow/common.h"
#include "protoactor/actor.h"

namespace protoactor {
class PID;
class Context;
}  // namespace protoactor

namespace agentflow {
namespace actor {

/// @brief Message for workflow status query
struct WorkflowStatusMessage {
  WorkflowId    workflow_id;
  WorkflowState state;
  DataMap       output_data;
  DataMap       metadata;

  WorkflowStatusMessage(const WorkflowId& id, WorkflowState s, const DataMap& output,
                        const DataMap& meta = DataMap())
      : workflow_id(id), state(s), output_data(output), metadata(meta) {}
};

/// @brief Workflow Actor for orchestrating workflow execution
class WorkflowActor : public protoactor::Actor, public AgentFlowActor {
public:
  /// @brief Constructor
  /// @param workflow_id The ID of workflow
  /// @param workflow_config Configuration for workflow
  WorkflowActor(const WorkflowId& workflow_id, const WorkflowConfig& workflow_config);

  /// @brief Destructor
  ~WorkflowActor() override = default;

  /// @brief Main message processing method (Protoactor interface)
  /// @param context The actor context
  void Receive(std::shared_ptr<protoactor::Context> context) override;

  /// @brief Process AgentFlow messages
  /// @param message_type Type of message
  /// @param message The message payload
  void process_message(const std::string& message_type, std::shared_ptr<void> message) override;

  // Getters
  const WorkflowId&     get_workflow_id() const { return workflow_id_; }
  const WorkflowConfig& get_config() const { return config_; }
  WorkflowState         get_state() const { return state_; }

private:
  WorkflowId                                         workflow_id_;
  WorkflowConfig                                     config_;
  WorkflowState                                      state_ = WorkflowState::CREATED;
  std::map<NodeId, std::shared_ptr<protoactor::PID>> node_actors_;
  DataMap                                            input_data_;
  DataMap                                            output_data_;
  std::unordered_set<NodeId>                         completed_nodes_;
  std::unordered_set<NodeId>                         failed_nodes_;

  /// @brief Handle ExecuteWorkflowMessage
  void handle_execute_workflow(const ExecuteWorkflowMessage& msg);

  /// @brief Handle NodeResultMessage from a child node actor
  void handle_node_result(const NodeResultMessage& msg);

  /// @brief Handle workflow status query
  void handle_status_query(std::shared_ptr<protoactor::Context> context);

  /// @brief Create and spawn a node actor
  /// @param node_id The node ID
  /// @return PID of spawned node actor
  std::shared_ptr<protoactor::PID> spawn_node_actor(const NodeId& node_id);

  /// @brief Schedule executable nodes (no unsatisfied dependencies)
  void schedule_executable_nodes();

  /// @brief Check if workflow has completed
  void check_workflow_completion();

  /// @brief Send workflow result to parent
  void send_workflow_result(WorkflowState state);

  /// @brief Convert WorkflowState to string
  static std::string workflow_state_to_string(WorkflowState state);
};

}  // namespace actor
}  // namespace agentflow

#else

// Stub for when Protoactor is not available
#include "agentflow/actor/actors.h"
#include "agentflow/common.h"

namespace agentflow {
namespace actor {

class WorkflowActor : public AgentFlowActor {
public:
  WorkflowActor(const WorkflowId& workflow_id, const WorkflowConfig& workflow_config) {
    (void)workflow_id;
    (void)workflow_config;
  }
  ~WorkflowActor() override = default;

  void process_message(const std::string& message_type, std::shared_ptr<void> message) override {
    (void)message_type;
    (void)message;
  }
};

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_WORKFLOW_ACTOR_H
