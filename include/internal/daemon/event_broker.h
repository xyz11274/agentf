// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// EventBroker - Event subscription and notification for agentflowd

#ifndef AGENTFLOW_DAEMON_EVENT_BROKER_H
#define AGENTFLOW_DAEMON_EVENT_BROKER_H

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace agentflow {
namespace daemon {

class RpcServer;

struct EventSubscription {
  int client_fd = -1;
  std::set<std::string> event_types;
};

class EventBroker {
public:
  EventBroker();
  ~EventBroker();

  void start(RpcServer& server);
  void stop();

  // Subscribe a client to event types
  int subscribe(int client_fd, const std::set<std::string>& event_types);
  // Unsubscribe a client (all or specific subscription)
  bool unsubscribe(int subscription_id);
  // Unsubscribe all subscriptions for a client
  void unsubscribe_client(int client_fd);

  // Called when an event occurs internally
  void emit_event(const std::string& event_type, const std::string& payload_json);

  bool is_running() const { return running_.load(); }

private:
  void poll_loop();

  RpcServer* rpc_server_ = nullptr;
  std::atomic<bool> running_{false};
  std::thread poll_thread_;

  int next_sub_id_ = 0;
  std::map<int, EventSubscription> subscriptions_;
  std::mutex subscriptions_mutex_;

  // Pending events to dispatch
  struct PendingEvent {
    std::string event_type;
    std::string payload_json;
  };
  std::vector<PendingEvent> pending_events_;
  std::mutex events_mutex_;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_EVENT_BROKER_H
