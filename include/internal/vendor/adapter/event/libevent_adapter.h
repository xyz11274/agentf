#pragma once

#include "vendor/adapter/event/event_adapter.h"
#include <event2/event.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace agentflow {
namespace vendor {
namespace adapter {

/**
 * LibEventAdapter implements EventAdapter using libevent.
 * Provides event subscription and publishing with libevent integration.
 */
class LibEventAdapter : public EventAdapter {
 public:
  LibEventAdapter();
  ~LibEventAdapter() override;

  std::string Subscribe(EventType event_type, EventHandler handler) override;
  void Unsubscribe(const std::string& subscription_id) override;
  void Publish(const EventData& event) override;
  bool Initialize() override;
  void Shutdown() override;
  bool IsInitialized() const override;

 private:
  // Helper to generate unique subscription IDs
  std::string GenerateSubscriptionId();

  // Data structure to hold subscription info
  struct Subscription {
    EventType type;
    EventHandler handler;
    std::string id;
  };

  // Members
  event_base* event_base_;  // libevent event base
  std::map<std::string, Subscription> subscriptions_;  // ID -> Subscription mapping
  std::map<EventType, std::vector<std::string>> type_to_ids_;  // EventType -> subscription IDs
  mutable std::mutex subscriptions_mutex_;
  uint64_t next_subscription_id_;
  bool initialized_;
};

}  // namespace adapter
}  // namespace vendor
}  // namespace agentflow
