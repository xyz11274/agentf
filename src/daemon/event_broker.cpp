// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// EventBroker implementation

#include "daemon/event_broker.h"

#include <chrono>

#include "daemon/rpc_server.h"
#include "utils/logger.h"

namespace agentflow {
namespace daemon {

EventBroker::EventBroker() = default;

EventBroker::~EventBroker() {
  stop();
}

void EventBroker::start(RpcServer& server) {
  if (running_.load()) return;
  rpc_server_ = &server;
  running_ = true;
  poll_thread_ = std::thread(&EventBroker::poll_loop, this);
  Logger::instance().info("[EVENT_BROKER] Started");
}

void EventBroker::stop() {
  if (!running_.exchange(false)) return;
  if (poll_thread_.joinable()) poll_thread_.join();
  Logger::instance().info("[EVENT_BROKER] Stopped");
}

int EventBroker::subscribe(int client_fd, const std::set<std::string>& event_types) {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);
  int sub_id = ++next_sub_id_;
  EventSubscription sub;
  sub.client_fd = client_fd;
  sub.event_types = event_types;
  subscriptions_[sub_id] = std::move(sub);
  Logger::instance().info_f("[EVENT_BROKER] Client fd=%d subscribed to %zu event types (sub_id=%d)",
                            client_fd, event_types.size(), sub_id);
  return sub_id;
}

bool EventBroker::unsubscribe(int subscription_id) {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);
  auto it = subscriptions_.find(subscription_id);
  if (it == subscriptions_.end()) return false;
  int fd = it->second.client_fd;
  subscriptions_.erase(it);
  Logger::instance().info_f("[EVENT_BROKER] Subscription %d removed (fd=%d)", subscription_id, fd);
  return true;
}

void EventBroker::unsubscribe_client(int client_fd) {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);
  for (auto it = subscriptions_.begin(); it != subscriptions_.end(); ) {
    if (it->second.client_fd == client_fd) {
      it = subscriptions_.erase(it);
    } else {
      ++it;
    }
  }
}

void EventBroker::emit_event(const std::string& event_type, const std::string& payload_json) {
  std::lock_guard<std::mutex> lock(events_mutex_);
  pending_events_.push_back({event_type, payload_json});
}

void EventBroker::poll_loop() {
  while (running_.load()) {
    // Swap pending events
    std::vector<PendingEvent> events;
    {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events.swap(pending_events_);
    }

    // Dispatch each event to matching subscribers
    for (auto& evt : events) {
      std::lock_guard<std::mutex> lock(subscriptions_mutex_);
      for (auto& [sub_id, sub] : subscriptions_) {
        if (sub.event_types.count(evt.event_type) || sub.event_types.count("*")) {
          std::string params = "{\"event_type\":\"" + evt.event_type
                             + "\",\"data\":" + evt.payload_json
                             + ",\"subscription_id\":" + std::to_string(sub_id) + "}";
          rpc_server_->send_notification(sub.client_fd, "event.notification", params);
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace daemon
}  // namespace agentflow
