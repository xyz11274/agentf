#include "agent/task.h"

#include <chrono>

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  ).count();
}
}  // namespace

MMAgentTask::MMAgentTask(const Config& config) : config_(config), state_(TaskState::PENDING) {
  created_time_ = get_current_time_ms();
}

MMAgentTask::~MMAgentTask() {
  stop_message_threads();
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.clear();
  intent_request_messages_.clear();
  data_exchange_messages_.clear();
  response_messages_.clear();
  dispatch_records_.clear();
}

bool MMAgentTask::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != TaskState::PENDING) {
    return false;
  }
  state_ = TaskState::RUNNING;
  return true;
}

bool MMAgentTask::complete(const std::string& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != TaskState::RUNNING) {
    return false;
  }
  state_ = TaskState::COMPLETED;
  result_ = result;
  completed_time_ = get_current_time_ms();
  progress_ = 1.0;
  return true;
}

bool MMAgentTask::fail(const std::string& error_message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == TaskState::COMPLETED || state_ == TaskState::CANCELLED) {
    return false;
  }
  state_ = TaskState::FAILED;
  error_message_ = error_message;
  completed_time_ = get_current_time_ms();
  return true;
}

bool MMAgentTask::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == TaskState::COMPLETED) {
    return false;
  }
  state_ = TaskState::CANCELLED;
  completed_time_ = get_current_time_ms();
  return true;
}

bool MMAgentTask::retry() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(retry_count_ >= config_.max_retries) {
    return false;
  }
  ++retry_count_;
  state_ = TaskState::RUNNING;
  return true;
}

void MMAgentTask::set_progress(double progress) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(progress >= 0.0 && progress <= 1.0) {
    progress_ = progress;
  }
}

void MMAgentTask::increment_progress(double delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_ = std::min(1.0, progress_ + delta);
}

void MMAgentTask::set_parameter(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_[key] = value;
}

bool MMAgentTask::get_parameter(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = parameters_.find(key);
  if(it == parameters_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

void MMAgentTask::set_result(const std::string& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  result_ = result;
}

int MMAgentTask::increment_retry_count() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ++retry_count_;
}

int64_t MMAgentTask::now_ms() {
  return get_current_time_ms();
}

bool MMAgentTask::start_message_threads(MessageHandler intent_handler, MessageHandler data_handler) {
  bool expected = false;
  if(!message_threads_running_.compare_exchange_strong(expected, true)) {
    return false;
  }
  configure_message_handlers(std::move(intent_handler), std::move(data_handler));
  intent_request_thread_ = std::thread(&MMAgentTask::intent_request_loop, this);
  data_exchange_thread_ = std::thread(&MMAgentTask::data_exchange_loop, this);
  monitor_thread_ = std::thread(&MMAgentTask::monitor_loop, this);
  return true;
}

void MMAgentTask::configure_message_handlers(MessageHandler intent_handler,
                                             MessageHandler data_handler) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    intent_request_handler_ = std::move(intent_handler);
    data_exchange_handler_ = std::move(data_handler);
  }
}

void MMAgentTask::stop_message_threads() {
  bool expected = true;
  if(!message_threads_running_.compare_exchange_strong(expected, false)) {
    return;
  }
  {
    std::unique_lock<std::mutex> lock(mutex_);
    queue_cv_.notify_all();
  }
  if(intent_request_thread_.joinable()) {
    intent_request_thread_.join();
  }
  if(data_exchange_thread_.joinable()) {
    data_exchange_thread_.join();
  }
  if(monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
}

bool MMAgentTask::enqueue_intent_request_message(const MMAgentMsgQue::Message& message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    intent_request_messages_.push_back(message);
  }
  queue_cv_.notify_all();
  return true;
}

bool MMAgentTask::enqueue_data_exchange_message(const MMAgentMsgQue::Message& message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    data_exchange_messages_.push_back(message);
    if(!message.message_id.empty()) {
      dispatch_records_.erase(message.message_id);
    }
  }
  queue_cv_.notify_all();
  return true;
}

int MMAgentTask::get_intent_request_queue_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(intent_request_messages_.size());
}

int MMAgentTask::get_data_exchange_queue_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(data_exchange_messages_.size());
}

bool MMAgentTask::enqueue_response_message(const MMAgentMsgQue::Message& message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    response_messages_.push_back(message);
  }
  queue_cv_.notify_all();
  return true;
}

bool MMAgentTask::dequeue_response_message(MMAgentMsgQue::Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(response_messages_.empty()) {
    return false;
  }
  message = response_messages_.front();
  response_messages_.pop_front();
  return true;
}

int MMAgentTask::get_response_queue_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(response_messages_.size());
}

void MMAgentTask::register_dispatch_record(const MMAgentMsgQue::Message& message) {
  if(message.message_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  DispatchRecord& record = dispatch_records_[message.message_id];
  record.message = message;
  record.last_sent_time_ms = now_ms();
  record.awaiting_response = true;
}

void MMAgentTask::acknowledge_dispatch_record(const std::string& message_id) {
  if(message_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  dispatch_records_.erase(message_id);
}

int MMAgentTask::get_running_skills_count() const {
  return running_skills_count_.load();
}

bool MMAgentTask::can_launch_more_skills() const {
  return running_skills_count_.load() < config_.max_concurrent_skills;
}

void MMAgentTask::on_skill_started(const std::string& skill_name) {
  (void)skill_name;
  running_skills_count_.fetch_add(1);
}

void MMAgentTask::on_skill_completed(const std::string& skill_name) {
  (void)skill_name;
  running_skills_count_.fetch_sub(1);
}

void MMAgentTask::clear_running_skills() {
  running_skills_count_.store(0);
}

int MMAgentTask::process_intent_request_queue_once() {
  MMAgentMsgQue::Message message;
  MessageHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(intent_request_messages_.empty()) {
      return 0;
    }
    message = intent_request_messages_.front();
    intent_request_messages_.pop_front();
    handler = intent_request_handler_;
  }
  if(handler) {
    handler(message);
  }
  return 1;
}

int MMAgentTask::process_data_exchange_queue_once() {
  MMAgentMsgQue::Message message;
  MessageHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(data_exchange_messages_.empty()) {
      return 0;
    }
    message = data_exchange_messages_.front();
    data_exchange_messages_.pop_front();
    handler = data_exchange_handler_;
  }
  if(handler) {
    handler(message);
  }
  return 1;
}

int MMAgentTask::process_pending_messages_once() {
  return process_intent_request_queue_once() + process_data_exchange_queue_once();
}

void MMAgentTask::intent_request_loop() {
  while(message_threads_running_) {
    if(process_intent_request_queue_once() == 0) {
      std::unique_lock<std::mutex> lock(mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(20), [this]() {
        return !message_threads_running_ || !intent_request_messages_.empty();
      });
    }
  }
}

void MMAgentTask::data_exchange_loop() {
  while(message_threads_running_) {
    if(process_data_exchange_queue_once() == 0) {
      std::unique_lock<std::mutex> lock(mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(20), [this]() {
        return !message_threads_running_ || !data_exchange_messages_.empty();
      });
    }
  }
}

void MMAgentTask::monitor_loop() {
  constexpr int64_t kResendIntervalMs = 500;
  while(message_threads_running_) {
    std::vector<MMAgentMsgQue::Message> retries;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const int64_t current = now_ms();
      for(auto& [message_id, record] : dispatch_records_) {
        if(!record.awaiting_response) {
          continue;
        }
        if((current - record.last_sent_time_ms) < kResendIntervalMs) {
          continue;
        }
        if(record.resend_count >= config_.max_retries) {
          continue;
        }
        record.last_sent_time_ms = current;
        ++record.resend_count;
        retries.push_back(record.message);
      }
    }
    for(const auto& retry_message : retries) {
      MessageHandler handler;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = intent_request_handler_;
      }
      if(handler) {
        handler(retry_message);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace framework
}  // namespace agentflow
