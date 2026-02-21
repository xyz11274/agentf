#include "agent/event.h"

#include <mutex>
#include <algorithm>

namespace agentflow {
namespace framework {

AgentEvent::AgentEvent(const Config& config)
    : config_(config) {
}

AgentEvent::~AgentEvent() {
  std::lock_guard<std::mutex> lock(mutex_);
  listeners_.clear();
  event_history_.clear();
}

bool AgentEvent::emit_event(const EventData& event_data) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  // Store in history
  event_history_.push_back(event_data);
  total_events_emitted_++;

  // Call registered listeners
  auto it = listeners_.find(event_data.type);
  if(it != listeners_.end()) {
    for(const auto& listener_pair : it->second) {
      listener_pair.second(event_data);
    }
  }
  return true;
}

bool AgentEvent::emit_event_async(const EventData& event_data) {
  // For now, just emit synchronously
  // In a real implementation, this would queue the event for async processing
  return emit_event(event_data);
}

int AgentEvent::register_listener(EventType type, EventListener listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(get_listener_count() >= config_.max_event_listeners) {
    return -1;
  }
  int listener_id = next_listener_id_++;
  listeners_[type].push_back({listener_id, listener});
  return listener_id;
}

bool AgentEvent::unregister_listener(int listener_id) {
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

bool AgentEvent::unregister_all_listeners(EventType type) {
  std::lock_guard<std::mutex> lock(mutex_);
  return listeners_.erase(type) > 0;
}

std::vector<AgentEvent::EventData> AgentEvent::get_recent_events(int count) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<EventData> result;
  int start_idx = std::max(0, static_cast<int>(event_history_.size()) - count);
  for(int i = start_idx; i < static_cast<int>(event_history_.size()); ++i) {
    result.push_back(event_history_[i]);
  }
  return result;
}

std::vector<AgentEvent::EventData> AgentEvent::get_events_by_type(EventType type, int count) const {
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

void AgentEvent::clear_events() {
  std::lock_guard<std::mutex> lock(mutex_);
  event_history_.clear();
}

int AgentEvent::get_listener_count() const {
  int count = 0;
  for(const auto& listener_pair : listeners_) {
    count += listener_pair.second.size();
  }
  return count;
}

}  // namespace framework
}  // namespace agentflow
