#include "agent/task.h"

#include <sys/time.h>

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}
}  // namespace

MMAgentTask::MMAgentTask(const Config& config) : config_(config), state_(TaskState::PENDING) {
  created_time_ = get_current_time_ms();
}

MMAgentTask::~MMAgentTask() {
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.clear();
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

}  // namespace framework
}  // namespace agentflow
