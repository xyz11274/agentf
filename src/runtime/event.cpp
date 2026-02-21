// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Event system implementation

#include "vendor/event/event.h"
#include "vendor/adapter/event/libevent_adapter.h"

#include <chrono>
#include <sstream>
#include <memory>

namespace agentflow {

uint64_t WorkflowEvent::get_current_timestamp_ms() {
  using namespace std::chrono;
  auto now = high_resolution_clock::now();
  auto ms  = duration_cast<milliseconds>(now.time_since_epoch());
  return ms.count();
}

EventManager::EventManager()
    : adapter_(vendor::adapter::CreateLibEventAdapter()) {
  // Adapter is created and initialized
  if(adapter_) {
    adapter_->Initialize();
  }
}

std::string EventManager::subscribe(EventType event_type, const EventHandler& handler) {
  if(!adapter_) {
    return "";
  }
  
  // Map agentflow::EventType to adapter::EventType
  vendor::adapter::EventType adapter_event_type = MapEventType(event_type);
  
  // Create a wrapper handler that converts adapter::EventData back to WorkflowEvent
  auto wrapper = [handler](const vendor::adapter::EventData& event_data) {
    // Extract the WorkflowEvent from custom_data
    if(event_data.custom_data) {
      try {
        // The custom_data contains a shared_ptr<WorkflowEvent>
        auto event_ptr = std::static_pointer_cast<WorkflowEvent>(event_data.custom_data);
        if(event_ptr) {
          handler(*event_ptr);
        }
      } catch(const std::exception& e) {
        // Silently ignore wrapper conversion errors
      }
    }
  };
  
  // Subscribe to the adapter with the wrapped handler
  std::lock_guard<std::mutex> lock(handler_mutex_);
  std::string subscription_id = adapter_->Subscribe(adapter_event_type, wrapper);
  return subscription_id;
}

void EventManager::unsubscribe(const std::string& subscription_id) {
  if(!adapter_) {
    return;
  }
  
  std::lock_guard<std::mutex> lock(handler_mutex_);
  adapter_->Unsubscribe(subscription_id);
}

void EventManager::publish(const WorkflowEvent& event) {
  if(!adapter_) {
    return;
  }
  
  // Map agentflow::EventType to adapter::EventType
  vendor::adapter::EventType adapter_event_type = MapEventType(event.get_event_type());
  
  // Create a copy of the WorkflowEvent and store it in a shared_ptr
  auto event_copy = std::make_shared<WorkflowEvent>(event);
  
  // Create EventData with the native event in custom_data
  vendor::adapter::EventData event_data;
  event_data.type = adapter_event_type;
  event_data.timestamp = event.get_timestamp_ms();
  event_data.custom_data = std::static_pointer_cast<void>(event_copy);
  // data field is left empty as we're using custom_data instead
  
  std::lock_guard<std::mutex> lock(handler_mutex_);
  try {
    adapter_->Publish(event_data);
  } catch(const std::exception& e) {
    // Silently ignore publish errors to prevent cascade failures
  }
}

size_t EventManager::get_subscriber_count(EventType event_type) const {
  // For now, return 0 as the adapter doesn't provide this interface
  // This could be extended in the future if needed
  return 0;
}

void EventManager::clear() {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  // The adapter manages subscriptions internally
  // We could add a Clear() method to EventAdapter if needed
}

std::string EventManager::generate_subscription_id() {
  std::stringstream ss;
  ss << "sub_" << (++next_subscription_id_);
  return ss.str();
}

}  // namespace agentflow
