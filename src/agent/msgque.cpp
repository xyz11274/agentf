#include "agent/msgque.h"

#include <mutex>

namespace agentflow {
namespace framework {

AgentMsgQue::AgentMsgQue(const Config& config) : config_(config) {
}

AgentMsgQue::~AgentMsgQue() {
  clear_queue();
}

bool AgentMsgQue::enqueue_to_priority_queue(const Message& message) {
  std::vector<Message>* target = &normal_priority_queue_;
  if(message.priority == MessagePriority::HIGH) {
    target = &high_priority_queue_;
  } else if(message.priority == MessagePriority::LOW) {
    target = &low_priority_queue_;
  }

  target->push_back(message);
  total_messages_processed_++;
  return true;
}

bool AgentMsgQue::enqueue_message(const Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(get_queue_size() >= config_.max_queue_size) {
    return false;
  }
  return enqueue_to_priority_queue(message);
}

bool AgentMsgQue::dequeue_from_priority_queues(Message& message) {
  if(!high_priority_queue_.empty()) {
    message = high_priority_queue_.front();
    high_priority_queue_.erase(high_priority_queue_.begin());
    return true;
  }
  if(!normal_priority_queue_.empty()) {
    message = normal_priority_queue_.front();
    normal_priority_queue_.erase(normal_priority_queue_.begin());
    return true;
  }
  if(!low_priority_queue_.empty()) {
    message = low_priority_queue_.front();
    low_priority_queue_.erase(low_priority_queue_.begin());
    return true;
  }
  return false;
}

bool AgentMsgQue::dequeue_message(Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  return dequeue_from_priority_queues(message);
}

bool AgentMsgQue::peek_message(Message& message) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(!high_priority_queue_.empty()) {
    message = high_priority_queue_.front();
    return true;
  }
  if(!normal_priority_queue_.empty()) {
    message = normal_priority_queue_.front();
    return true;
  }
  if(!low_priority_queue_.empty()) {
    message = low_priority_queue_.front();
    return true;
  }
  return false;
}

int AgentMsgQue::get_queue_size() const {
  return static_cast<int>(high_priority_queue_.size() + normal_priority_queue_.size() +
                          low_priority_queue_.size());
}

int AgentMsgQue::get_queue_size(MessagePriority priority) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(priority == MessagePriority::HIGH) {
    return static_cast<int>(high_priority_queue_.size());
  }
  if(priority == MessagePriority::LOW) {
    return static_cast<int>(low_priority_queue_.size());
  }
  return static_cast<int>(normal_priority_queue_.size());
}

bool AgentMsgQue::is_queue_empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return get_queue_size() == 0;
}

bool AgentMsgQue::is_queue_full() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return get_queue_size() >= config_.max_queue_size;
}

void AgentMsgQue::clear_queue() {
  std::lock_guard<std::mutex> lock(mutex_);
  high_priority_queue_.clear();
  normal_priority_queue_.clear();
  low_priority_queue_.clear();
}

bool AgentMsgQue::route_message(const std::string& receiver_id, const Message& message) {
  Message routed_message = message;
  routed_message.receiver_id = receiver_id;
  return enqueue_message(routed_message);
}

std::vector<AgentMsgQue::Message> AgentMsgQue::get_messages_for_receiver(
    const std::string& receiver_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Message> result;
  auto collect = [&result, &receiver_id](const std::vector<Message>& queue) {
    for(const auto& msg : queue) {
      if(msg.receiver_id == receiver_id) {
        result.push_back(msg);
      }
    }
  };
  collect(high_priority_queue_);
  collect(normal_priority_queue_);
  collect(low_priority_queue_);
  return result;
}

}  // namespace framework
}  // namespace agentflow
