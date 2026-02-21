#include "agent/session.h"

#include <sys/time.h>
#include <mutex>

#include "agent/task.h"

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
}  // namespace

AgentSession::AgentSession(const Config& config)
    : config_(config),
      state_(SessionState::CREATED),
      receive_queue_(AgentMsgQue::Config{config.max_queue_size}),
      send_queue_(AgentMsgQue::Config{config.max_queue_size}) {
  created_time_ = get_current_time_ms();
  last_activity_time_ = created_time_;
}

AgentSession::~AgentSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_.clear();
  tasks_.clear();
}

bool AgentSession::activate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != SessionState::CREATED) {
    return false;
  }
  state_ = SessionState::ACTIVE;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool AgentSession::pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != SessionState::ACTIVE) {
    return false;
  }
  state_ = SessionState::PAUSED;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool AgentSession::resume() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != SessionState::PAUSED) {
    return false;
  }
  state_ = SessionState::ACTIVE;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool AgentSession::complete() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == SessionState::TERMINATED || state_ == SessionState::COMPLETED) {
    return false;
  }
  state_ = SessionState::COMPLETED;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool AgentSession::terminate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == SessionState::TERMINATED) {
    return false;
  }
  state_ = SessionState::TERMINATED;
  last_activity_time_ = get_current_time_ms();
  return true;
}

void AgentSession::set_metadata(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_[key] = value;
}

bool AgentSession::get_metadata(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = metadata_.find(key);
  if(it != metadata_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

bool AgentSession::has_metadata(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metadata_.count(key) > 0;
}

void AgentSession::clear_metadata() {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_.clear();
}

void AgentSession::update_activity() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_activity_time_ = get_current_time_ms();
}

bool AgentSession::is_timeout_expired() const {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t current_time = get_current_time_ms();
  return (current_time - last_activity_time_) > config_.timeout_ms;
}

bool AgentSession::enqueue_received_message(const AgentMsgQue::Message& message, bool high_priority) {
  AgentMsgQue::Message queued = message;
  queued.priority = high_priority ? AgentMsgQue::MessagePriority::HIGH : queued.priority;
  update_activity();
  return receive_queue_.enqueue_message(queued);
}

bool AgentSession::dequeue_received_message(AgentMsgQue::Message& message) {
  bool ok = receive_queue_.dequeue_message(message);
  if(ok) {
    update_activity();
  }
  return ok;
}

bool AgentSession::enqueue_send_message(const AgentMsgQue::Message& message) {
  update_activity();
  return send_queue_.enqueue_message(message);
}

bool AgentSession::dequeue_send_message(AgentMsgQue::Message& message) {
  bool ok = send_queue_.dequeue_message(message);
  if(ok) {
    update_activity();
  }
  return ok;
}

int AgentSession::get_receive_queue_size() const {
  return receive_queue_.get_queue_size();
}

int AgentSession::get_send_queue_size() const {
  return send_queue_.get_queue_size();
}

void AgentSession::add_task(const std::shared_ptr<AgentTask>& task) {
  if(!task) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_[task->get_task_id()] = task;
}

std::shared_ptr<AgentTask> AgentSession::get_task(const std::string& task_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = tasks_.find(task_id);
  if(it == tasks_.end()) {
    return nullptr;
  }
  return it->second;
}

std::vector<std::shared_ptr<AgentTask>> AgentSession::get_tasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<AgentTask>> result;
  result.reserve(tasks_.size());
  for(const auto& kv : tasks_) {
    result.push_back(kv.second);
  }
  return result;
}

}  // namespace framework
}  // namespace agentflow
