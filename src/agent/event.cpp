#include "agent/event.h"
#include "vendor/adapter/event/libevent_adapter.h"

#include <mutex>
#include <algorithm>
#include <iostream>

#include "agent/dataref_bridge.h"

namespace agentflow {
namespace framework {

using AdapterEventType = agentflow::vendor::adapter::EventType;

AdapterEventType MapToAdapterEventType(MMAgentEvent::EventType type) {
    switch (type) {
        case MMAgentEvent::EventType::TASK_STARTED: return AdapterEventType::TASK_STARTED;
        case MMAgentEvent::EventType::TASK_COMPLETED: return AdapterEventType::TASK_COMPLETED;
        case MMAgentEvent::EventType::TASK_FAILED: return AdapterEventType::TASK_FAILED;
        case MMAgentEvent::EventType::MESSAGE_SENT: return AdapterEventType::AGENT_MESSAGE;
        case MMAgentEvent::EventType::MESSAGE_RECEIVED: return AdapterEventType::AGENT_MESSAGE;
        
        case MMAgentEvent::EventType::NODE_START: return AdapterEventType::NODE_START;
        case MMAgentEvent::EventType::NODE_COMPLETE: return AdapterEventType::NODE_COMPLETE;
        case MMAgentEvent::EventType::NODE_FAILURE: return AdapterEventType::NODE_FAILURE;
        case MMAgentEvent::EventType::NODE_RETRY: return AdapterEventType::NODE_RETRY;
        case MMAgentEvent::EventType::DATA_READY: return AdapterEventType::DATA_READY;
        case MMAgentEvent::EventType::SYNC_WAIT: return AdapterEventType::SYNC_WAIT;
        case MMAgentEvent::EventType::DEPENDENCY_READY: return AdapterEventType::DEPENDENCY_READY;
        
        default: return AdapterEventType::CUSTOM;
    }
}

MMAgentEvent::MMAgentEvent(const Config& config)
    : config_(config) {
    if (config_.async_dispatch) {
        adapter_ = agentflow::vendor::adapter::CreateLibEventAdapter();
        if (adapter_) {
            adapter_->Initialize();
        }
    }
}

MMAgentEvent::~MMAgentEvent() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (adapter_) {
      adapter_->Shutdown();
  }
  listeners_.clear();
  event_history_.clear();
}

bool MMAgentEvent::emit_event(const EventData& event_data) {
  std::lock_guard<std::mutex> lock(mutex_);
  EventData normalized = event_data;
  if(normalized.data_ref.empty() && !normalized.description.empty()) {
    normalized.data_ref =
        dataref_bridge::store_payload(normalized.description, normalized.attributes,
                                      "event:" + normalized.event_id);
    if(!normalized.data_ref.empty()) {
      normalized.description.clear();
    }
  }
  
  // Store in history
  event_history_.push_back(normalized);
  total_events_emitted_++;

  // Call registered listeners
  auto it = listeners_.find(normalized.type);
  if(it != listeners_.end()) {
    for(const auto& listener_pair : it->second) {
      listener_pair.second(normalized);
    }
  }
  return true;
}

bool MMAgentEvent::emit_event_async(const EventData& event_data) {
  if (config_.async_dispatch && adapter_) {
      agentflow::vendor::adapter::EventData adapter_data;
      adapter_data.type = MapToAdapterEventType(event_data.type);
      adapter_data.timestamp = event_data.timestamp;
      // We pass the original event data via custom_data to retrieve it back
      adapter_data.custom_data = std::make_shared<EventData>(event_data);
      
      adapter_->Publish(adapter_data);
      return true;
  }
  return emit_event(event_data);
}

int MMAgentEvent::register_listener(EventType type, EventListener listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(get_listener_count() >= config_.max_event_listeners) {
    return -1;
  }
  int listener_id = next_listener_id_++;
  bool first_listener = listeners_[type].empty();
  listeners_[type].push_back({listener_id, listener});
  
  // If async dispatch is enabled and this is the first listener for this type,
  // subscribe to the adapter to receive events of this type.
  if (config_.async_dispatch && adapter_ && first_listener) {
      AdapterEventType adapter_type = MapToAdapterEventType(type);
      
      auto self = this;
      adapter_->Subscribe(adapter_type, [self](const agentflow::vendor::adapter::EventData& data) {
          if (data.custom_data) {
              auto event_ptr = std::static_pointer_cast<EventData>(data.custom_data);
              if (event_ptr) {
                  // We call emit_event directly, which will notify all listeners
                  self->emit_event(*event_ptr);
              }
          }
      });
  }
  
  return listener_id;
}

bool MMAgentEvent::unregister_listener(int listener_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for(auto& listener_pair : listeners_) {
    auto& listener_list = listener_pair.second;
    auto it = std::find_if(listener_list.begin(), listener_list.end(),
                          [listener_id](const std::pair<int, EventListener>& p) {
                            return p.first == listener_id;
                          });
    if(it != listener_list.end()) {
      listener_list.erase(it);
      return true;
    }
  }
  return false;
}

bool MMAgentEvent::unregister_all_listeners(EventType type) {
  std::lock_guard<std::mutex> lock(mutex_);
  return listeners_.erase(type) > 0;
}

std::vector<MMAgentEvent::EventData> MMAgentEvent::get_recent_events(int count) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<EventData> result;
  int start_idx = std::max(0, static_cast<int>(event_history_.size()) - count);
  for(int i = start_idx; i < static_cast<int>(event_history_.size()); ++i) {
    result.push_back(event_history_[i]);
  }
  return result;
}

std::vector<MMAgentEvent::EventData> MMAgentEvent::get_events_by_type(EventType type, int count) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<EventData> result;
  for(const auto& event : event_history_) {
    if(event.type == type) {
      result.push_back(event);
    }
    if(static_cast<int>(result.size()) >= count) {
      break;
    }
  }
  return result;
}

void MMAgentEvent::clear_events() {
  std::lock_guard<std::mutex> lock(mutex_);
  event_history_.clear();
}

int MMAgentEvent::get_listener_count() const {
  int count = 0;
  for(const auto& listener_pair : listeners_) {
    count += listener_pair.second.size();
  }
  return count;
}

}  // namespace framework
}  // namespace agentflow
