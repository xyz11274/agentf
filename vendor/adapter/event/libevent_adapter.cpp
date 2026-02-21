#include "vendor/adapter/event/libevent_adapter.h"
#include <chrono>
#include <sstream>
#include <algorithm>

namespace agentflow {
namespace vendor {
namespace adapter {

LibEventAdapter::LibEventAdapter()
    : event_base_(nullptr),
      next_subscription_id_(0),
      initialized_(false) {}

LibEventAdapter::~LibEventAdapter() {
  if (initialized_) {
    Shutdown();
  }
}

bool LibEventAdapter::Initialize() {
  if (initialized_) {
    return true;
  }

  // Create a new event base
  event_base_ = event_base_new();
  if (!event_base_) {
    return false;
  }

  initialized_ = true;
  return true;
}

void LibEventAdapter::Shutdown() {
  if (!initialized_) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_.clear();
    type_to_ids_.clear();
  }

  if (event_base_) {
    // Free any pending events (though we don't actively schedule any yet)
    event_base_free(event_base_);
    event_base_ = nullptr;
  }

  initialized_ = false;
}

bool LibEventAdapter::IsInitialized() const {
  return initialized_;
}

std::string LibEventAdapter::GenerateSubscriptionId() {
  std::ostringstream oss;
  oss << "sub_" << (++next_subscription_id_);
  return oss.str();
}

std::string LibEventAdapter::Subscribe(EventType event_type, EventHandler handler) {
  if (!initialized_) {
    return "";
  }

  std::lock_guard<std::mutex> lock(subscriptions_mutex_);

  std::string subscription_id = GenerateSubscriptionId();
  Subscription sub;
  sub.type = event_type;
  sub.handler = handler;
  sub.id = subscription_id;

  subscriptions_[subscription_id] = sub;
  type_to_ids_[event_type].push_back(subscription_id);

  return subscription_id;
}

void LibEventAdapter::Unsubscribe(const std::string& subscription_id) {
  if (!initialized_) {
    return;
  }

  std::lock_guard<std::mutex> lock(subscriptions_mutex_);

  auto it = subscriptions_.find(subscription_id);
  if (it == subscriptions_.end()) {
    return;
  }

  EventType event_type = it->second.type;
  subscriptions_.erase(it);

  // Remove from type mapping
  auto type_it = type_to_ids_.find(event_type);
  if (type_it != type_to_ids_.end()) {
    auto& ids = type_it->second;
    ids.erase(std::remove(ids.begin(), ids.end(), subscription_id), ids.end());
    if (ids.empty()) {
      type_to_ids_.erase(type_it);
    }
  }
}

void LibEventAdapter::Publish(const EventData& event) {
  if (!initialized_) {
    return;
  }

  std::lock_guard<std::mutex> lock(subscriptions_mutex_);

  // Find all handlers for this event type
  auto it = type_to_ids_.find(event.type);
  if (it == type_to_ids_.end()) {
    return;  // No subscribers for this event type
  }

  const auto& subscription_ids = it->second;
  for (const auto& sub_id : subscription_ids) {
    auto sub_it = subscriptions_.find(sub_id);
    if (sub_it != subscriptions_.end()) {
      // Invoke the handler
      try {
        sub_it->second.handler(event);
      } catch (const std::exception& e) {
        // Log the exception but continue with other handlers
        // TODO: Add proper logging
      }
    }
  }
}

std::unique_ptr<EventAdapter> CreateLibEventAdapter() {
  auto adapter = std::make_unique<LibEventAdapter>();
  if (adapter->Initialize()) {
    return adapter;
  }
  return nullptr;
}

}  // namespace adapter
}  // namespace vendor
}  // namespace agentflow
