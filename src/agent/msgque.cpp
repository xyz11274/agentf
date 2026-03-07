#include "agent/msgque.h"

#include <mutex>
#include <sstream>

#include "agent/dataref_bridge.h"

namespace agentflow {
namespace framework {

MMAgentMsgQue::MMAgentMsgQue(const Config& config) : config_(config) {
}

MMAgentMsgQue::~MMAgentMsgQue() {
  clear_queue();
}

MMAgentMsgQue::MessageQueueType MMAgentMsgQue::resolve_queue_type(const Message& message) const {
  const auto header_type_it = message.metadata.find("message_type");
  if(header_type_it != message.metadata.end()) {
    if(header_type_it->second == "data_exchange" || header_type_it->second == "output_result"
       || header_type_it->second == "notification" || header_type_it->second == "error") {
      return MessageQueueType::DATA_EXCHANGE;
    }
    return MessageQueueType::INTENT_REQUEST;
  }

  if(message.type == MessageType::TASK_RESPONSE || message.type == MessageType::NOTIFICATION
     || message.type == MessageType::ERROR) {
    return MessageQueueType::DATA_EXCHANGE;
  }
  return MessageQueueType::INTENT_REQUEST;
}

bool MMAgentMsgQue::enqueue_to_type_queue(const Message& message) {
  std::vector<Message>* target = &intent_request_queue_;
  const auto queue_type = resolve_queue_type(message);
  if(queue_type == MessageQueueType::DATA_EXCHANGE) {
    target = &data_exchange_queue_;
  }
  target->push_back(message);
  total_messages_processed_++;
  return true;
}

bool MMAgentMsgQue::enqueue_message(const Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(get_queue_size() >= config_.max_queue_size) {
    return false;
  }
  Message control_message = message;
  if(control_message.data_ref.empty() && !control_message.content.empty()) {
    std::ostringstream logical_key;
    logical_key << "msg:" << control_message.message_id;
    control_message.data_ref =
        dataref_bridge::store_payload(control_message.content, control_message.metadata, logical_key.str());
    if(!control_message.data_ref.empty()) {
      control_message.content.clear();
    }
  }
  return enqueue_to_type_queue(control_message);
}

bool MMAgentMsgQue::dequeue_from_type_queue(MessageQueueType queue_type, Message& message) {
  std::vector<Message>* queue = &intent_request_queue_;
  if(queue_type == MessageQueueType::DATA_EXCHANGE) {
    queue = &data_exchange_queue_;
  }
  if(!queue->empty()) {
    message = queue->front();
    queue->erase(queue->begin());
    return true;
  }
  return false;
}

bool MMAgentMsgQue::dequeue_message(Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(dequeue_from_type_queue(MessageQueueType::INTENT_REQUEST, message)) {
    return true;
  }
  return dequeue_from_type_queue(MessageQueueType::DATA_EXCHANGE, message);
}

bool MMAgentMsgQue::dequeue_message_by_type(MessageQueueType queue_type, Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  return dequeue_from_type_queue(queue_type, message);
}

bool MMAgentMsgQue::peek_message(Message& message) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(!intent_request_queue_.empty()) {
    message = intent_request_queue_.front();
    return true;
  }
  if(!data_exchange_queue_.empty()) {
    message = data_exchange_queue_.front();
    return true;
  }
  return false;
}

int MMAgentMsgQue::get_queue_size() const {
  return static_cast<int>(intent_request_queue_.size() + data_exchange_queue_.size());
}

int MMAgentMsgQue::get_queue_size(MessageQueueType queue_type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(queue_type == MessageQueueType::DATA_EXCHANGE) {
    return static_cast<int>(data_exchange_queue_.size());
  }
  return static_cast<int>(intent_request_queue_.size());
}

bool MMAgentMsgQue::is_queue_empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return get_queue_size() == 0;
}

bool MMAgentMsgQue::is_queue_full() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return get_queue_size() >= config_.max_queue_size;
}

void MMAgentMsgQue::clear_queue() {
  std::lock_guard<std::mutex> lock(mutex_);
  intent_request_queue_.clear();
  data_exchange_queue_.clear();
}

bool MMAgentMsgQue::route_message(const std::string& receiver_id, const Message& message) {
  Message routed_message = message;
  routed_message.receiver_id = receiver_id;
  return enqueue_message(routed_message);
}

std::vector<MMAgentMsgQue::Message> MMAgentMsgQue::get_messages_for_receiver(
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
  collect(intent_request_queue_);
  collect(data_exchange_queue_);
  return result;
}

}  // namespace framework
}  // namespace agentflow
