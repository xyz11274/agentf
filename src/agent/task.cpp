#include "agent/task.h"

#include <sys/time.h>
#include <mutex>

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
}  // namespace

AgentTask::AgentTask(const Config& config)
    : config_(config), state_(TaskState::PENDING) {
  created_time_ = get_current_time_ms();
}

AgentTask::~AgentTask() {
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.clear();
}

bool AgentTask::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != TaskState::PENDING) {
    return false;
  }
  state_ = TaskState::RUNNING;
  return true;
}

bool AgentTask::complete(const std::string& result) {
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

bool AgentTask::fail(const std::string& error_message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == TaskState::COMPLETED || state_ == TaskState::CANCELLED) {
    return false;
  }
  state_ = TaskState::FAILED;
  error_message_ = error_message;
  completed_time_ = get_current_time_ms();
  return true;
}

bool AgentTask::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == TaskState::COMPLETED) {
    return false;
  }
  state_ = TaskState::CANCELLED;
  completed_time_ = get_current_time_ms();
  return true;
}

bool AgentTask::retry() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(retry_count_ >= config_.max_retries) {
    return false;
  }
  retry_count_++;
  state_ = TaskState::RUNNING;
  return true;
}

void AgentTask::set_progress(double progress) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(progress >= 0.0 && progress <= 1.0) {
    progress_ = progress;
  }
}

void AgentTask::increment_progress(double delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_ = std::min(1.0, progress_ + delta);
}

void AgentTask::set_parameter(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_[key] = value;
}

bool AgentTask::get_parameter(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = parameters_.find(key);
  if(it != parameters_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

void AgentTask::set_result(const std::string& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  result_ = result;
}

int AgentTask::increment_retry_count() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ++retry_count_;
}

}  // namespace framework
}  // namespace agentflow
