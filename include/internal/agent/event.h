#ifndef AGENTFLOW_INTERNAL_AGENT_EVENT_H
#define AGENTFLOW_INTERNAL_AGENT_EVENT_H

#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <mutex>

namespace agentflow {
namespace vendor {
namespace adapter {
class EventAdapter;
}
}
}

namespace agentflow {
namespace framework {

/**
 * AgentEvent handles event generation and dispatching for agent operations
 * Supports event listeners, handlers, and event routing
 */
class AgentEvent {
public:
  enum class EventType {
    SESSION_CREATED  = 0,
    SESSION_CLOSED   = 1,
    TASK_STARTED     = 2,
    TASK_COMPLETED   = 3,
    TASK_FAILED      = 4,
    MESSAGE_SENT     = 5,
    MESSAGE_RECEIVED = 6,
    ERROR_OCCURRED   = 7,
    STATE_CHANGED    = 8,
    // Workflow events
    NODE_START       = 9,
    NODE_COMPLETE    = 10,
    NODE_FAILURE     = 11,
    NODE_RETRY       = 12,
    DATA_READY       = 13,
    SYNC_WAIT        = 14,
    DEPENDENCY_READY = 15,
  };

  struct EventData {
    std::string                          event_id;
    EventType                            type;
    std::string                          source_id;
    std::string                          target_id;
    std::string                          description;
    std::string                          data_ref;  // Databus reference for payload
    int64_t                              timestamp = 0;
    std::map<std::string, std::string>   attributes;
  };

  struct Config {
    std::string handler_id;
    int         max_event_listeners = 100;
    bool        async_dispatch = false;
  };

  explicit AgentEvent(const Config& config);
  ~AgentEvent();

  // Event emission
  bool emit_event(const EventData& event_data);
  bool emit_event_async(const EventData& event_data);

  // Event listeners
  using EventListener = std::function<void(const EventData&)>;
  int  register_listener(EventType type, EventListener listener);
  bool unregister_listener(int listener_id);
  bool unregister_all_listeners(EventType type);

  // Event retrieval
  std::vector<EventData> get_recent_events(int count = 10) const;
  std::vector<EventData> get_events_by_type(EventType type, int count = 10) const;
  void                   clear_events();

  // Configuration
  const Config& get_config() const { return config_; }
  int64_t       get_total_events_emitted() const { return total_events_emitted_; }
  int           get_listener_count() const;

private:
  Config                                      config_;
  std::map<EventType, std::vector<std::pair<int, EventListener>>> listeners_;
  std::vector<EventData>                      event_history_;
  int64_t                                     total_events_emitted_ = 0;
  int                                         next_listener_id_ = 1;
  mutable std::mutex                          mutex_;
  
  // LibEvent adapter for async dispatch
  std::unique_ptr<agentflow::vendor::adapter::EventAdapter> adapter_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_EVENT_H
