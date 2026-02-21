#include "agent/messages.h"

#include <mutex>
#include <algorithm>

namespace agentflow {
namespace framework {

AgentMessages::AgentMessages(const Config& config)
    : config_(config) {
}

AgentMessages::~AgentMessages() {
  std::lock_guard<std::mutex> lock(mutex_);
  message_queue_.clear();
}

bool AgentMessages::enqueue_message(const Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(static_cast<int>(message_queue_.size()) >= config_.max_queue_size) {
    return false;
  }
  message_queue_.push_back(message);
  total_messages_processed_++;
  return true;
}

bool AgentMessages::dequeue_message(Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(message_queue_.empty()) {
    return false;
  }
  message = message_queue_.front();
  message_queue_.erase(message_queue_.begin());
  return true;
}

bool AgentMessages::peek_message(Message& message) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(message_queue_.empty()) {
    return false;
  }
  message = message_queue_.front();
  return true;
}

int AgentMessages::get_queue_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_queue_.size();
}

bool AgentMessages::is_queue_empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_queue_.empty();
}

bool AgentMessages::is_queue_full() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(message_queue_.size()) >= config_.max_queue_size;
}

void AgentMessages::clear_queue() {
  std::lock_guard<std::mutex> lock(mutex_);
  message_queue_.clear();
}

bool AgentMessages::route_message(const std::string& receiver_id, const Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(static_cast<int>(message_queue_.size()) >= config_.max_queue_size) {
    return false;
  }
  Message routed_message = message;
  routed_message.receiver_id = receiver_id;
  message_queue_.push_back(routed_message);
  total_messages_processed_++;
  return true;
}

std::vector<AgentMessages::Message> AgentMessages::get_messages_for_receiver(
    const std::string& receiver_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Message> result;
  for(const auto& msg : message_queue_) {
    if(msg.receiver_id == receiver_id) {
      result.push_back(msg);
    }
  }
  return result;
}

}  // namespace framework
}  // namespace agentflow
