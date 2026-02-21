#include "runtime/worker.h"

#include <sys/time.h>

namespace agentflow {
namespace runtime {

int64_t WorkerEngine::now_ms() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

WorkerEngine::WorkerEngine(const Config& config) : config_(config) {
  last_heartbeat_ms_ = now_ms();
}

WorkerEngine::WorkerStatus WorkerEngine::get_status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

int WorkerEngine::get_load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_;
}

int64_t WorkerEngine::get_last_heartbeat_ms() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_heartbeat_ms_;
}

void WorkerEngine::set_status(WorkerStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = status;
}

void WorkerEngine::set_load(int load) {
  std::lock_guard<std::mutex> lock(mutex_);
  load_ = load < 0 ? 0 : load;
}

void WorkerEngine::heartbeat() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_heartbeat_ms_ = now_ms();
}

void WorkerEngine::set_supported_workflows(const std::set<std::string>& workflow_ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_workflows_ = workflow_ids;
}

void WorkerEngine::add_supported_workflow(const std::string& workflow_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_workflows_.insert(workflow_id);
}

void WorkerEngine::remove_supported_workflow(const std::string& workflow_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_workflows_.erase(workflow_id);
}

bool WorkerEngine::supports_workflow(const std::string& workflow_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return supported_workflows_.empty() || supported_workflows_.count(workflow_id) > 0;
}

std::set<std::string> WorkerEngine::get_supported_workflows() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return supported_workflows_;
}

}  // namespace runtime
}  // namespace agentflow
