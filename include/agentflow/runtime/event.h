// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// This file defines the event system for control flow management

#ifndef AGENTFLOW_RUNTIME_EVENT_H
#define AGENTFLOW_RUNTIME_EVENT_H

#include <string>
#include <memory>
#include <functional>
#include <map>
#include <vector>
#include <mutex>

#include "agentflow/common.h"

namespace agentflow {

// Forward declarations
class Workflow;
class Node;

/// @brief Represents an event in the workflow execution pipeline
class WorkflowEvent {
 public:
  /// @brief Constructor
  /// @param event_type The type of event
  /// @param node_id The node that generated the event
  /// @param workflow_id The workflow associated with the event
  /// @param data Optional event data
  explicit WorkflowEvent(EventType event_type,
                         const NodeId& node_id,
                         const WorkflowId& workflow_id,
                         const DataMap& data = DataMap())
      : event_type_(event_type), node_id_(node_id),
        workflow_id_(workflow_id), data_(data),
        timestamp_ms_(get_current_timestamp_ms()) {}

  // Getters
  EventType get_event_type() const { return event_type_; }
  const NodeId& get_node_id() const { return node_id_; }
  const WorkflowId& get_workflow_id() const { return workflow_id_; }
  const DataMap& get_data() const { return data_; }
  uint64_t get_timestamp_ms() const { return timestamp_ms_; }

 private:
  EventType event_type_;
  NodeId node_id_;
  WorkflowId workflow_id_;
  DataMap data_;
  uint64_t timestamp_ms_;

  static uint64_t get_current_timestamp_ms();
};

// Event handler callback type
using EventHandler = std::function<void(const WorkflowEvent&)>;

/// @brief Event manager for workflow control flow
class EventManager {
 public:
  /// @brief Constructor
  EventManager() = default;

  /// @brief Destructor
  ~EventManager() = default;

  /// @brief Subscribe to events
  /// @param event_type The type of event to subscribe to
  /// @param handler The callback handler
  /// @return Subscription ID
  std::string subscribe(EventType event_type, const EventHandler& handler);

  /// @brief Unsubscribe from events
  /// @param subscription_id The subscription ID returned from subscribe
  void unsubscribe(const std::string& subscription_id);

  /// @brief Publish an event
  /// @param event The event to publish
  void publish(const WorkflowEvent& event);

  /// @brief Get number of subscribers for an event type
  /// @param event_type The event type
  /// @return Number of subscribers
  size_t get_subscriber_count(EventType event_type) const;

  /// @brief Clear all subscriptions
  void clear();

 private:
  struct Subscription {
    EventType event_type;
    EventHandler handler;
  };

  mutable std::mutex handler_mutex_;
  std::map<std::string, Subscription> handlers_;
  size_t next_subscription_id_ = 0;

  std::string generate_subscription_id();
};

}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_EVENT_H
