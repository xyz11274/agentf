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
    if(header_type_it->second == "output_result") {
      return MessageQueueType::OUTPUT_RESULT;
    }
    if(header_type_it->second == "extensible_reserved") {
      return MessageQueueType::EXTENSIBLE_RESERVED;
    }
    return MessageQueueType::INTENT_REQUEST;
  }

  if(message.type == MessageType::TASK_RESPONSE) {
    return MessageQueueType::OUTPUT_RESULT;
  }
  return MessageQueueType::INTENT_REQUEST;
}

bool MMAgentMsgQue::enqueue_to_type_queue(const Message& message) {
  std::vector<Message>* target = &intent_request_queue_;
  const auto queue_type = resolve_queue_type(message);
  if(queue_type == MessageQueueType::OUTPUT_RESULT) {
    target = &output_result_queue_;
  } else if(queue_type == MessageQueueType::EXTENSIBLE_RESERVED) {
    target = &extensible_reserved_queue_;
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
  if(queue_type == MessageQueueType::OUTPUT_RESULT) {
    queue = &output_result_queue_;
  } else if(queue_type == MessageQueueType::EXTENSIBLE_RESERVED) {
    queue = &extensible_reserved_queue_;
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
  if(dequeue_from_type_queue(MessageQueueType::OUTPUT_RESULT, message)) {
    return true;
  }
  return dequeue_from_type_queue(MessageQueueType::EXTENSIBLE_RESERVED, message);
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
  if(!output_result_queue_.empty()) {
    message = output_result_queue_.front();
    return true;
  }
  if(!extensible_reserved_queue_.empty()) {
    message = extensible_reserved_queue_.front();
    return true;
  }
  return false;
}

int MMAgentMsgQue::get_queue_size() const {
  return static_cast<int>(intent_request_queue_.size() + output_result_queue_.size() +
                          extensible_reserved_queue_.size());
}

int MMAgentMsgQue::get_queue_size(MessageQueueType queue_type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(queue_type == MessageQueueType::OUTPUT_RESULT) {
    return static_cast<int>(output_result_queue_.size());
  }
  if(queue_type == MessageQueueType::EXTENSIBLE_RESERVED) {
    return static_cast<int>(extensible_reserved_queue_.size());
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
  output_result_queue_.clear();
  extensible_reserved_queue_.clear();
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
  collect(output_result_queue_);
  collect(extensible_reserved_queue_);
  return result;
}

}  // namespace framework
}  // namespace agentflow
