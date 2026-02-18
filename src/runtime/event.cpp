// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Event system implementation

#include "runtime/event.h"

#include <chrono>
#include <sstream>

namespace agentflow {

uint64_t WorkflowEvent::get_current_timestamp_ms() {
  using namespace std::chrono;
  auto now = high_resolution_clock::now();
  auto ms  = duration_cast<milliseconds>(now.time_since_epoch());
  return ms.count();
}

std::string EventManager::subscribe(EventType event_type, const EventHandler& handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  std::string                 subscription_id = generate_subscription_id();
  handlers_[subscription_id]                  = { event_type, handler };
  return subscription_id;
}

void EventManager::unsubscribe(const std::string& subscription_id) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  handlers_.erase(subscription_id);
}

void EventManager::publish(const WorkflowEvent& event) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  for(const auto& pair : handlers_) {
    const Subscription& sub = pair.second;
    if(sub.event_type == event.get_event_type()) {
      try {
        sub.handler(event);
      } catch(const std::exception& e) {
        // Silently ignore handler exceptions to prevent cascade failures
      }
    }
  }
}

size_t EventManager::get_subscriber_count(EventType event_type) const {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  size_t                      count = 0;
  for(const auto& pair : handlers_) {
    if(pair.second.event_type == event_type) {
      ++count;
    }
  }
  return count;
}

void EventManager::clear() {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  handlers_.clear();
  next_subscription_id_ = 0;
}

std::string EventManager::generate_subscription_id() {
  std::stringstream ss;
  ss << "sub_" << (++next_subscription_id_);
  return ss.str();
}

}  // namespace agentflow
