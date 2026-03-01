#include "runtime/master/scheduler.h"

#include <algorithm>
#include <chrono>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "utils/logger.h"

namespace agentflow {
namespace runtime {
namespace master {

namespace {
using PluginFn = bool (*)(const TaskScheduleRequest&, const std::vector<WorkflowInstanceInfo>&,
                          const std::map<std::string, TaskScheduleResult>&, TaskScheduleResult&);
}  // namespace

Scheduler::Scheduler() : Scheduler(Config{}) {
}

Scheduler::Scheduler(const Config& config) : config_(config) {
  start();
  if(!config_.plugin_so_path.empty()) {
    load_plugin_algorithm(config_.plugin_so_path);
  }
}

Scheduler::~Scheduler() {
  stop();
#if defined(__unix__) || defined(__APPLE__)
  if(plugin_handle_) {
    dlclose(plugin_handle_);
    plugin_handle_ = nullptr;
  }
#endif
}

bool Scheduler::start() {
  bool expected = false;
  if(!running_.compare_exchange_strong(expected, true)) {
    return false;
  }
  scheduling_thread_ = std::thread(&Scheduler::scheduling_loop, this);
  return true;
}

void Scheduler::stop() {
  bool expected = true;
  if(!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  queue_cv_.notify_all();
  if(scheduling_thread_.joinable()) {
    scheduling_thread_.join();
  }
}

void Scheduler::set_workflow_snapshot_provider(
    std::function<std::vector<WorkflowInstanceInfo>()> provider) {
  std::lock_guard<std::mutex> lock(mapping_mutex_);
  workflow_snapshot_provider_ = std::move(provider);
}

TaskScheduleResult Scheduler::run_default_algorithm(
    const TaskScheduleRequest& request, const std::vector<WorkflowInstanceInfo>& workflow_instances,
    const std::map<std::string, TaskScheduleResult>& stable_mapping) const {
  (void)stable_mapping;
  TaskScheduleResult result;

  const WorkflowInstanceInfo* best_same_instance = nullptr;
  const WorkflowInstanceInfo* best_same_object = nullptr;
  const WorkflowInstanceInfo* best_any = nullptr;
  int best_same_instance_load = 0;
  int best_same_object_load = 0;
  int best_any_load = 0;

  for(const auto& wi : workflow_instances) {
    if(wi.workflow_id != request.workflow_id) {
      continue;
    }
    if(!request.workflow_file.empty() && !wi.workflow_file.empty()
       && wi.workflow_file != request.workflow_file) {
      continue;
    }
    int current_load = wi.ref_count;
    auto load_it = workflow_task_load_.find(wi.workflow_instance_id);
    if(load_it != workflow_task_load_.end()) {
      current_load += load_it->second;
    }

    if(!best_any || current_load < best_any_load) {
      best_any = &wi;
      best_any_load = current_load;
    }
    if(wi.agent_object_id == request.agent_object_id) {
      if(!best_same_object || current_load < best_same_object_load) {
        best_same_object = &wi;
        best_same_object_load = current_load;
      }
      if(wi.agent_instance_id == request.agent_instance_id
         && (!best_same_instance || current_load < best_same_instance_load)) {
        best_same_instance = &wi;
        best_same_instance_load = current_load;
      }
    }
  }

  const WorkflowInstanceInfo* chosen =
      best_same_instance ? best_same_instance : (best_same_object ? best_same_object : best_any);
  if(!chosen) {
    return result;
  }

  result.scheduled = true;
  result.workflow_instance_id = chosen->workflow_instance_id;
  result.worker_id = chosen->worker_id;
  return result;
}

TaskScheduleResult Scheduler::schedule_task(const TaskScheduleRequest& request,
                                            const std::vector<WorkflowInstanceInfo>& workflow_instances) {
  std::lock_guard<std::mutex> lock(mapping_mutex_);
  auto result = algorithm_ ? algorithm_(request, workflow_instances, task_to_workflow_)
                           : run_default_algorithm(request, workflow_instances, task_to_workflow_);
  if(result.scheduled) {
    task_to_workflow_[request.task_id] = result;
    ++workflow_task_load_[result.workflow_instance_id];
    task_status_[request.task_id] = "scheduled";
  }
  return result;
}

bool Scheduler::enqueue_task(const TaskScheduleRequest& request) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    task_queue_.push(request);
  }
  queue_cv_.notify_all();
  return true;
}

std::map<std::string, TaskScheduleResult> Scheduler::list_task_assignments() const {
  std::lock_guard<std::mutex> lock(mapping_mutex_);
  return task_to_workflow_;
}

bool Scheduler::handle_agent_state_change(const std::string& agent_instance_id, bool is_healthy) {
  if(is_healthy) {
    return true;
  }
  std::lock_guard<std::mutex> lock(mapping_mutex_);
  for(auto it = task_to_workflow_.begin(); it != task_to_workflow_.end();) {
    if(it->first.find(agent_instance_id + ":") == 0) {
      auto load_it = workflow_task_load_.find(it->second.workflow_instance_id);
      if(load_it != workflow_task_load_.end() && load_it->second > 0) {
        --load_it->second;
      }
      it = task_to_workflow_.erase(it);
    } else {
      ++it;
    }
  }
  return true;
}

bool Scheduler::update_task_status(const std::string& task_id, const std::string& status, int progress) {
  (void)progress;
  std::lock_guard<std::mutex> lock(mapping_mutex_);
  auto map_it = task_to_workflow_.find(task_id);
  if(map_it == task_to_workflow_.end()) {
    return false;
  }
  task_status_[task_id] = status;
  if((status == "completed" || status == "cancelled") && map_it->second.workflow_instance_id.size() > 0) {
    auto load_it = workflow_task_load_.find(map_it->second.workflow_instance_id);
    if(load_it != workflow_task_load_.end() && load_it->second > 0) {
      --load_it->second;
    }
  }
  return true;
}

bool Scheduler::load_plugin_algorithm(const std::string& so_path) {
#if defined(__unix__) || defined(__APPLE__)
  if(plugin_handle_) {
    dlclose(plugin_handle_);
    plugin_handle_ = nullptr;
  }
  void* handle = dlopen(so_path.c_str(), RTLD_NOW);
  if(!handle) {
    Logger::instance().warn_f("Scheduler plugin load failed: %s", so_path.c_str());
    algorithm_ = SchedulerAlgorithm();
    return false;
  }
  PluginFn fn = reinterpret_cast<PluginFn>(dlsym(handle, "agentflow_scheduler_compute"));
  if(!fn) {
    Logger::instance().warn_f("Scheduler plugin symbol missing: %s", so_path.c_str());
    dlclose(handle);
    algorithm_ = SchedulerAlgorithm();
    return false;
  }
  plugin_handle_ = handle;
  algorithm_ = [fn](const TaskScheduleRequest& request,
                    const std::vector<WorkflowInstanceInfo>& workflow_instances,
                    const std::map<std::string, TaskScheduleResult>& stable_mapping) {
    TaskScheduleResult out;
    if(!fn(request, workflow_instances, stable_mapping, out)) {
      return TaskScheduleResult{};
    }
    return out;
  };
  return true;
#else
  (void)so_path;
  algorithm_ = SchedulerAlgorithm();
  return false;
#endif
}

void Scheduler::scheduling_loop() {
  while(running_) {
    TaskScheduleRequest request;
    bool has_request = false;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(config_.schedule_interval_ms),
                         [this]() { return !task_queue_.empty() || !running_; });
      if(!running_) {
        break;
      }
      if(!task_queue_.empty()) {
        request = task_queue_.front();
        task_queue_.pop();
        has_request = true;
      }
    }
    if(!has_request) {
      continue;
    }
    std::vector<WorkflowInstanceInfo> snapshot;
    {
      std::lock_guard<std::mutex> lock(mapping_mutex_);
      if(workflow_snapshot_provider_) {
        snapshot = workflow_snapshot_provider_();
      }
    }
    if(snapshot.empty()) {
      continue;
    }
    schedule_task(request, snapshot);
  }
}

}  // namespace master
}  // namespace runtime
}  // namespace agentflow
