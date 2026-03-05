#pragma once

#include <functional>
#include <map>
#include <string>
#include <memory>

namespace agentflow {
namespace vendor {
namespace adapter {

/**
 * EventType represents different event types in the system.
 * Used to categorize and dispatch events.
 */
enum class EventType {
  // MMWorkflow-related events
  WORKFLOW_STARTED   = 0,
  WORKFLOW_COMPLETED = 1,
  WORKFLOW_FAILED    = 2,
  
  // MMNode-related events
  NODE_START       = 3,
  NODE_COMPLETE    = 4,
  NODE_FAILURE     = 5,
  NODE_RETRY       = 6,
  
  // Data flow events
  DATA_READY       = 7,
  SYNC_WAIT        = 8,
  DEPENDENCY_READY = 9,
  
  // Legacy/Custom events
  TASK_CREATED      = 10,
  TASK_STARTED      = 11,
  TASK_COMPLETED    = 12,
  TASK_FAILED       = 13,
  AGENT_MESSAGE     = 14,
  CUSTOM            = 15
};

/**
 * EventData holds the data associated with an event.
 * The custom_data field can hold a pointer to the original native event object
 * (e.g., shared_ptr<WorkflowEvent>), allowing adapters to pass rich event data.
 */
struct EventData {
  EventType type;
  std::string data;
  uint64_t timestamp = 0;
  std::shared_ptr<void> custom_data;  // Holds native event data (e.g., shared_ptr<WorkflowEvent>)
};

/**
 * EventHandler is a callback function type for handling events.
 */
using EventHandler = std::function<void(const EventData&)>;

/**
 * EventAdapter provides an abstraction for event management.
 * Implementations can use different underlying mechanisms (libevent, callbacks, etc.)
 */
class EventAdapter {
 public:
  virtual ~EventAdapter() = default;

  /**
   * Subscribe to events of a specific type.
   * @param event_type The type of event to subscribe to.
   * @param handler The callback to invoke when an event is published.
   * @return A subscription ID that can be used to unsubscribe later.
   */
  virtual std::string Subscribe(EventType event_type, EventHandler handler) = 0;

  /**
   * Unsubscribe from events.
   * @param subscription_id The subscription ID returned from Subscribe.
   */
  virtual void Unsubscribe(const std::string& subscription_id) = 0;

  /**
   * Publish an event to all subscribed handlers.
   * @param event The event data to publish.
   */
  virtual void Publish(const EventData& event) = 0;

  /**
   * Initialize the adapter (e.g., create event_base for libevent).
   */
  virtual bool Initialize() = 0;

  /**
   * Shutdown the adapter and clean up resources.
   */
  virtual void Shutdown() = 0;

  /**
   * Check if the adapter is initialized.
   */
  virtual bool IsInitialized() const = 0;
};

/**
 * Create a libevent-based adapter.
 */
std::unique_ptr<EventAdapter> CreateLibEventAdapter();

}  // namespace adapter
}  // namespace vendor
}  // namespace agentflow
