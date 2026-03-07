#include "workflow/edge.h"

#include <algorithm>

#include "utils/logger.h"

namespace agentflow {
namespace framework {

MMEdge::MMEdge(const Config& config) : config_(config) {
}

MMEdge::~MMEdge() {
  stop_runtime();
}

bool MMEdge::start_runtime() {
  return start_runtime(RuntimeConfig{});
}

bool MMEdge::start_runtime(const RuntimeConfig& config) {
  bool expected = false;
  if(!runtime_running_.compare_exchange_strong(expected, true)) {
    return false;
  }

  runtime_config_ = config;
  processor_pool_ = std::make_unique<basic::ThreadPool>(
      static_cast<size_t>(std::max(1, runtime_config_.processor_threads)));

  message_thread_ = std::thread(&MMEdge::message_dispatch_loop, this);
  return true;
}

void MMEdge::stop_runtime() {
  bool expected = true;
  if(!runtime_running_.compare_exchange_strong(expected, false)) {
    return;
  }

  message_cv_.notify_all();

  if(message_thread_.joinable()) {
    message_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    high_priority_messages_.clear();
    normal_priority_messages_.clear();
  }

  processor_pool_.reset();
}

bool MMEdge::enqueue_message(const Message& message) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  const size_t queued_size = high_priority_messages_.size() + normal_priority_messages_.size();
  if(queued_size >= runtime_config_.max_queue_size) {
    return false;
  }

  if(message.priority == MessagePriority::HIGH) {
    high_priority_messages_.push_back(message);
  } else {
    normal_priority_messages_.push_back(message);
  }
  message_cv_.notify_all();
  return true;
}

bool MMEdge::enqueue_event(const EdgeEvent& event) {
  (void)event;
  return true;
}

void MMEdge::register_event_handler(const std::string& event_type, EdgeEventHandler handler) {
  (void)event_type;
  (void)handler;
}

void MMEdge::message_dispatch_loop() {
  while(runtime_running_) {
    Message message;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      message_cv_.wait(lock, [this]() {
        return !high_priority_messages_.empty() || !normal_priority_messages_.empty()
               || !runtime_running_;
      });
      if(!runtime_running_) {
        break;
      }

      if(!high_priority_messages_.empty()) {
        message = high_priority_messages_.front();
        high_priority_messages_.pop_front();
      } else if(!normal_priority_messages_.empty()) {
        message = normal_priority_messages_.front();
        normal_priority_messages_.pop_front();
      } else {
        continue;
      }
    }

    if(processor_pool_) {
      processor_pool_->submit([this, message]() { process_message(message); });
    } else {
      process_message(message);
    }
  }
}

void MMEdge::event_dispatch_loop() {
  // Edge event dispatch has been retired in favor of worker control/business queues.
}

void MMEdge::process_message(const Message& message) {
  DataMap target_data;
  const bool transferred = transfer_data(message.source_data, target_data);

  EdgeEvent event;
  event.type = transferred ? "edge_message_processed" : "edge_message_failed";
  event.data["message_id"] = message.message_id;
  event.data["edge_id"] = get_id();
  enqueue_event(event);

  Logger::instance().debug_f("[EDGE_PROCESS] edge=%s source=%s target=%s msg=%s",
                             get_id().c_str(), get_source().c_str(), get_target().c_str(),
                             message.message_id.c_str());
}

bool MMEdge::transfer_data(const DataMap& source_data, DataMap& target_data) {
  // Transfer data from source to target node
  // Based on configured data transfer mode

  for(size_t index = 0; index < config_.source_outputs.size(); ++index) {
    const auto& output = config_.source_outputs[index];
    auto it = source_data.find(output);
    if(it != source_data.end()) {
      const std::string target_key =
          index < config_.target_inputs.size() ? config_.target_inputs[index] : output;
      if(config_.transfer_mode == DataTransferMode::REFERENCE) {
        target_data[target_key] = std::string("databus://global/") + config_.source_node + "."
                                  + output;
      } else {
        target_data[target_key] = it->second;
      }
    }
  }

  return true;
}

bool MMEdge::trigger_event(const std::string& event_type) {
  // Trigger control flow event
  auto it = std::find(config_.supported_events.begin(), config_.supported_events.end(), event_type);
  return it != config_.supported_events.end();
}

}  // namespace framework
}  // namespace agentflow
