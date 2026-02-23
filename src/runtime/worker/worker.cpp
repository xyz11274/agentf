#include "runtime/worker.h"

#include <sys/time.h>

#include <algorithm>

#include "utils/logger.h"

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

WorkerEngine::~WorkerEngine() {
  stop();
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

bool WorkerEngine::start() {
  bool expected = false;
  if(!running_.compare_exchange_strong(expected, true)) {
    return false;
  }

  set_status(WorkerStatus::RUNNING);
  const int event_threads = std::max(1, config_.event_threads);
  event_pool_             = std::make_unique<basic::ThreadPool>(static_cast<size_t>(event_threads));
  event_monitor_thread_   = std::thread(&WorkerEngine::event_monitor_loop, this);
  return true;
}

void WorkerEngine::stop() {
  bool expected = true;
  if(!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  queue_cv_.notify_all();
  if(event_monitor_thread_.joinable()) {
    event_monitor_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while(!event_queue_.empty()) {
      event_queue_.pop();
    }
    workflow_registrations_ = std::queue<WorkflowRegistration>();
    high_priority_tasks_.clear();
    normal_priority_tasks_.clear();
  }

  workflow_pools_.clear();
  event_pool_.reset();
  set_status(WorkerStatus::STOPPED);
}

bool WorkerEngine::enqueue_task(const TaskItem& task) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  const size_t queued_tasks = high_priority_tasks_.size() + normal_priority_tasks_.size();
  if(queued_tasks >= config_.max_queue_size) {
    return false;
  }

  if(task.priority == TaskPriority::HIGH) {
    high_priority_tasks_.push_back(task);
  } else {
    normal_priority_tasks_.push_back(task);
  }

  WorkerEvent event;
  event.type          = "task_arrived";
  event.created_at_ms = now_ms();
  event.data["task_id"] = task.task_id;
  event.data["workflow_id"] = task.workflow_id;
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

bool WorkerEngine::register_workflow_instance(const WorkflowRegistration& registration) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if(workflow_registrations_.size() >= config_.max_queue_size) {
    return false;
  }

  workflow_registrations_.push(registration);
  WorkerEvent event;
  event.type                           = "workflow_registered";
  event.created_at_ms                  = now_ms();
  event.data["workflow_id"]            = registration.workflow_id;
  event.data["workflow_instance_id"]   = registration.workflow_instance_id;
  event.data["thread_count"]           = std::to_string(registration.thread_count);
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

bool WorkerEngine::unregister_workflow_instance(const std::string& workflow_instance_id) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  WorkerEvent event;
  event.type                         = "workflow_unregistered";
  event.created_at_ms                = now_ms();
  event.data["workflow_instance_id"] = workflow_instance_id;
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

bool WorkerEngine::enqueue_event(const WorkerEvent& event) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if(event_queue_.size() >= config_.max_queue_size) {
    return false;
  }
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

void WorkerEngine::register_event_handler(const std::string& event_type, WorkerEventHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  event_handlers_[event_type].push_back(std::move(handler));
}

std::string WorkerEngine::workflow_pool_key(const TaskItem& task) const {
  if(!task.workflow_instance_id.empty()) {
    return task.workflow_instance_id;
  }
  return task.workflow_id;
}

void WorkerEngine::ensure_workflow_pool(const std::string& pool_key, int thread_count) {
  const int effective_threads = std::max(1, thread_count <= 0 ? config_.default_workflow_threads : thread_count);
  auto      it                = workflow_pools_.find(pool_key);
  if(it == workflow_pools_.end()) {
    workflow_pools_[pool_key] =
        std::make_unique<basic::ThreadPool>(static_cast<size_t>(effective_threads));
  }
}

void WorkerEngine::handle_internal_event(const WorkerEvent& event) {
  if(event.type == "task_arrived") {
    process_task_queues();
    return;
  }

  if(event.type == "workflow_registered") {
    const std::string& workflow_id = event.data.count("workflow_instance_id") > 0
                                         ? event.data.at("workflow_instance_id")
                                         : event.data.at("workflow_id");
    int thread_count = config_.default_workflow_threads;
    auto thread_it = event.data.find("thread_count");
    if(thread_it != event.data.end()) {
      thread_count = std::stoi(thread_it->second);
    }
    ensure_workflow_pool(workflow_id, thread_count);
    return;
  }

  if(event.type == "workflow_unregistered") {
    auto it = event.data.find("workflow_instance_id");
    if(it != event.data.end()) {
      workflow_pools_.erase(it->second);
    }
  }
}

void WorkerEngine::process_task_queues() {
  std::vector<TaskItem> tasks_to_run;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while(!high_priority_tasks_.empty()) {
      tasks_to_run.push_back(high_priority_tasks_.front());
      high_priority_tasks_.pop_front();
    }
    while(!normal_priority_tasks_.empty()) {
      tasks_to_run.push_back(normal_priority_tasks_.front());
      normal_priority_tasks_.pop_front();
    }
  }

  for(const auto& task : tasks_to_run) {
    const std::string key = workflow_pool_key(task);
    if(key.empty()) {
      continue;
    }

    ensure_workflow_pool(key, config_.default_workflow_threads);
    auto pool_it = workflow_pools_.find(key);
    if(pool_it == workflow_pools_.end()) {
      continue;
    }

    auto* pool = pool_it->second.get();
    pool->submit([this, task]() {
      set_load(get_load() + 1);
      heartbeat();
      Logger::instance().debug_f("[WORKER_TASK] worker=%s workflow=%s task=%s",
                                 get_worker_id().c_str(), task.workflow_id.c_str(),
                                 task.task_id.c_str());
      set_load(get_load() - 1);
    });
  }
}

void WorkerEngine::event_monitor_loop() {
  while(running_) {
    WorkerEvent event;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() { return !event_queue_.empty() || !running_; });
      if(!running_) {
        break;
      }
      event = event_queue_.front();
      event_queue_.pop();
    }

    handle_internal_event(event);

    std::vector<WorkerEventHandler> handlers;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      auto                        it = event_handlers_.find(event.type);
      if(it != event_handlers_.end()) {
        handlers = it->second;
      }
    }

    for(const auto& handler : handlers) {
      if(event_pool_) {
        event_pool_->submit([handler, event]() { handler(event); });
      } else {
        handler(event);
      }
    }
  }
}

}  // namespace runtime
}  // namespace agentflow
