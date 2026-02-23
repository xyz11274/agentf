#include "runtime/master/deployer.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "utils/logger.h"

namespace agentflow {
namespace runtime {
namespace master {

namespace {
using PluginFn = bool (*)(const std::vector<WorkflowInstanceInfo>&, const std::vector<WorkerSnapshot>&,
                          const std::map<std::string, std::string>&,
                          std::map<std::string, std::string>&);
}  // namespace

Deployer::Deployer() : Deployer(Config{}) {
}

Deployer::Deployer(const Config& config) : config_(config) {
  start();
  if(!config_.plugin_so_path.empty()) {
    load_plugin_algorithm(config_.plugin_so_path);
  }
}

Deployer::~Deployer() {
  stop();
#if defined(__unix__) || defined(__APPLE__)
  if(plugin_handle_) {
    dlclose(plugin_handle_);
    plugin_handle_ = nullptr;
  }
#endif
}

bool Deployer::start() {
  bool expected = false;
  if(!running_.compare_exchange_strong(expected, true)) {
    return false;
  }
  registration_thread_ = std::thread(&Deployer::registration_loop, this);
  scheduling_thread_ = std::thread(&Deployer::scheduling_loop, this);
  return true;
}

void Deployer::stop() {
  bool expected = true;
  if(!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  queue_cv_.notify_all();
  if(registration_thread_.joinable()) {
    registration_thread_.join();
  }
  if(scheduling_thread_.joinable()) {
    scheduling_thread_.join();
  }
}

int64_t Deployer::now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Deployer::register_worker_internal(const std::shared_ptr<WorkerEngine>& worker) {
  if(!worker) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  workers_[worker->get_worker_id()] = worker;
  ++mapping_version_;
  return true;
}

bool Deployer::register_worker(const std::shared_ptr<WorkerEngine>& worker) {
  if(!worker) {
    return false;
  }
  {
    std::lock_guard<std::mutex> qlock(queue_mutex_);
    worker_registration_queue_.push({worker, false});
  }
  queue_cv_.notify_all();
  return register_worker_internal(worker);
}

bool Deployer::unregister_worker_internal(const std::string& worker_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(workers_.erase(worker_id) == 0) {
    return false;
  }
  for(auto it = workflow_to_worker_.begin(); it != workflow_to_worker_.end();) {
    if(it->second == worker_id) {
      it = workflow_to_worker_.erase(it);
    } else {
      ++it;
    }
  }
  ++mapping_version_;
  return true;
}

bool Deployer::unregister_worker(const std::string& worker_id) {
  {
    std::lock_guard<std::mutex> qlock(queue_mutex_);
    worker_registration_queue_.push({std::make_shared<WorkerEngine>(
                                         WorkerEngine::Config{worker_id, false}),
                                     true});
  }
  queue_cv_.notify_all();
  return unregister_worker_internal(worker_id);
}

bool Deployer::update_worker_heartbeat(const std::string& worker_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = workers_.find(worker_id);
  if(it == workers_.end()) {
    return false;
  }
  it->second->heartbeat();
  return true;
}

bool Deployer::update_worker_status(const std::string& worker_id, WorkerEngine::WorkerStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = workers_.find(worker_id);
  if(it == workers_.end()) {
    return false;
  }
  it->second->set_status(status);
  return true;
}

bool Deployer::update_worker_load(const std::string& worker_id, int load) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = workers_.find(worker_id);
  if(it == workers_.end()) {
    return false;
  }
  it->second->set_load(load);
  return true;
}

std::shared_ptr<WorkerEngine> Deployer::pick_worker_for_workflow(
    const std::string& workflow_id, const std::string& preferred_worker_id) const {
  if(!preferred_worker_id.empty()) {
    auto it = workers_.find(preferred_worker_id);
    if(it != workers_.end() && it->second->supports_workflow(workflow_id)
       && it->second->get_status() == WorkerEngine::WorkerStatus::RUNNING) {
      return it->second;
    }
  }

  std::shared_ptr<WorkerEngine> picked;
  int min_load = 0;
  for(const auto& kv : workers_) {
    const auto& worker = kv.second;
    if(worker->get_status() != WorkerEngine::WorkerStatus::RUNNING
       || !worker->supports_workflow(workflow_id)) {
      continue;
    }
    if(!picked || worker->get_load() < min_load) {
      picked = worker;
      min_load = worker->get_load();
    }
  }
  return picked;
}

bool Deployer::deploy_workflow_internal(const WorkflowInstanceInfo& info,
                                        const std::string& preferred_worker_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto worker = pick_worker_for_workflow(info.workflow_id, preferred_worker_id);
  if(!worker) {
    return false;
  }
  workflow_to_worker_[info.workflow_instance_id] = worker->get_worker_id();
  ++mapping_version_;
  return true;
}

bool Deployer::deploy_workflow_instance(const std::string& workflow_instance_id,
                                        const std::string& workflow_id,
                                        const std::string& preferred_worker_id) {
  WorkflowInstanceInfo info;
  info.workflow_instance_id = workflow_instance_id;
  info.workflow_id = workflow_id;
  return enqueue_deploy_request(info, preferred_worker_id) && deploy_workflow_internal(info, preferred_worker_id);
}

bool Deployer::enqueue_deploy_request(const WorkflowInstanceInfo& info,
                                      const std::string& preferred_worker_id) {
  {
    std::lock_guard<std::mutex> qlock(queue_mutex_);
    workflow_deploy_queue_.push({info, preferred_worker_id});
  }
  queue_cv_.notify_all();
  return true;
}

bool Deployer::remove_workflow_instance(const std::string& workflow_instance_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(workflow_to_worker_.erase(workflow_instance_id) == 0) {
    return false;
  }
  ++mapping_version_;
  return true;
}

bool Deployer::get_workflow_worker(const std::string& workflow_instance_id, std::string& worker_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = workflow_to_worker_.find(workflow_instance_id);
  if(it == workflow_to_worker_.end()) {
    return false;
  }
  worker_id = it->second;
  return true;
}

std::map<std::string, std::string> Deployer::get_all_deployments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workflow_to_worker_;
}

std::map<std::string, std::string> Deployer::run_default_algorithm(
    const std::vector<WorkflowInstanceInfo>& infos, const std::vector<WorkerSnapshot>& workers,
    const std::map<std::string, std::string>& stable_mapping) const {
  std::map<std::string, std::string> result = stable_mapping;
  std::map<std::string, int>         projected_load;
  for(const auto& ws : workers) {
    if(ws.status == WorkerEngine::WorkerStatus::RUNNING) {
      projected_load[ws.worker_id] = ws.load;
    }
  }

  for(const auto& info : infos) {
    auto stable_it = stable_mapping.find(info.workflow_instance_id);
    if(stable_it != stable_mapping.end() && projected_load.count(stable_it->second) > 0) {
      continue;
    }

    std::string preferred_worker;
    if(!info.agent_instance_id.empty()) {
      for(const auto& other : infos) {
        if(other.agent_instance_id == info.agent_instance_id
           && other.workflow_instance_id != info.workflow_instance_id) {
          auto m = result.find(other.workflow_instance_id);
          if(m != result.end() && projected_load.count(m->second) > 0) {
            preferred_worker = m->second;
            break;
          }
        }
      }
    }
    if(!preferred_worker.empty()) {
      result[info.workflow_instance_id] = preferred_worker;
      ++projected_load[preferred_worker];
      continue;
    }

    std::string chosen_worker;
    int chosen_load = 0;
    for(const auto& kv : projected_load) {
      if(chosen_worker.empty() || kv.second < chosen_load) {
        chosen_worker = kv.first;
        chosen_load = kv.second;
      }
    }
    if(!chosen_worker.empty()) {
      result[info.workflow_instance_id] = chosen_worker;
      ++projected_load[chosen_worker];
    }
  }

  return result;
}

bool Deployer::rebalance(const std::vector<WorkflowInstanceInfo>& infos) {
  auto workers = list_worker_snapshots();
  auto stable = get_all_deployments();
  auto computed = algorithm_ ? algorithm_(infos, workers, stable) : run_default_algorithm(infos, workers, stable);

  std::lock_guard<std::mutex> lock(mutex_);
  bool changed = false;
  for(const auto& kv : computed) {
    auto it = workflow_to_worker_.find(kv.first);
    if(it == workflow_to_worker_.end() || it->second != kv.second) {
      workflow_to_worker_[kv.first] = kv.second;
      changed = true;
    }
  }
  if(changed) {
    ++mapping_version_;
  }
  return true;
}

bool Deployer::handle_agent_state_change(const std::string& agent_instance_id, bool is_healthy) {
  if(is_healthy) {
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  bool changed = false;
  for(auto it = workflow_to_worker_.begin(); it != workflow_to_worker_.end();) {
    if(it->first.find(agent_instance_id + ":") == 0) {
      it = workflow_to_worker_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if(changed) {
    ++mapping_version_;
  }
  return true;
}

bool Deployer::load_plugin_algorithm(const std::string& so_path) {
#if defined(__unix__) || defined(__APPLE__)
  if(plugin_handle_) {
    dlclose(plugin_handle_);
    plugin_handle_ = nullptr;
  }
  void* handle = dlopen(so_path.c_str(), RTLD_NOW);
  if(!handle) {
    Logger::instance().warn_f("Deployer plugin load failed: %s", so_path.c_str());
    algorithm_ = DeployerAlgorithm();
    return false;
  }
  PluginFn fn = reinterpret_cast<PluginFn>(dlsym(handle, "agentflow_deployer_compute"));
  if(!fn) {
    Logger::instance().warn_f("Deployer plugin symbol missing: %s", so_path.c_str());
    dlclose(handle);
    algorithm_ = DeployerAlgorithm();
    return false;
  }
  plugin_handle_ = handle;
  algorithm_ = [fn](const std::vector<WorkflowInstanceInfo>& infos, const std::vector<WorkerSnapshot>& workers,
                    const std::map<std::string, std::string>& stable_mapping) {
    std::map<std::string, std::string> out;
    if(!fn(infos, workers, stable_mapping, out)) {
      return stable_mapping;
    }
    return out;
  };
  return true;
#else
  (void)so_path;
  algorithm_ = DeployerAlgorithm();
  return false;
#endif
}

std::vector<std::string> Deployer::find_unhealthy_workers_locked(int64_t now) const {
  std::vector<std::string> failed;
  for(const auto& kv : workers_) {
    const auto status = kv.second->get_status();
    if(status == WorkerEngine::WorkerStatus::STOPPED) {
      failed.push_back(kv.first);
      continue;
    }
    if(now - kv.second->get_last_heartbeat_ms() > config_.heartbeat_timeout_ms) {
      failed.push_back(kv.first);
    }
  }
  return failed;
}

std::vector<Deployer::WorkflowDeployRequest> Deployer::collect_failed_worker_workflows_locked(
    const std::vector<std::string>& failed_workers) const {
  std::set<std::string> failed(failed_workers.begin(), failed_workers.end());
  std::vector<WorkflowDeployRequest> requests;
  for(const auto& kv : workflow_to_worker_) {
    if(failed.count(kv.second) == 0) {
      continue;
    }
    WorkflowDeployRequest req;
    req.info.workflow_instance_id = kv.first;
    requests.push_back(req);
  }
  return requests;
}

void Deployer::registration_loop() {
  while(running_) {
    WorkerRegistrationRequest request;
    bool has_request = false;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(config_.schedule_interval_ms),
                         [this]() { return !worker_registration_queue_.empty() || !running_; });
      if(!running_) {
        break;
      }
      if(!worker_registration_queue_.empty()) {
        request = worker_registration_queue_.front();
        worker_registration_queue_.pop();
        has_request = true;
      }
    }
    if(!has_request) {
      continue;
    }
    if(request.unregister) {
      unregister_worker_internal(request.worker ? request.worker->get_worker_id() : "");
      continue;
    }
    register_worker_internal(request.worker);
  }
}

void Deployer::scheduling_loop() {
  while(running_) {
    std::vector<WorkflowDeployRequest> requests;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(config_.schedule_interval_ms),
                         [this]() { return !workflow_deploy_queue_.empty() || !running_; });
      if(!running_) {
        break;
      }
      while(!workflow_deploy_queue_.empty()) {
        requests.push_back(workflow_deploy_queue_.front());
        workflow_deploy_queue_.pop();
      }
    }

    for(const auto& req : requests) {
      deploy_workflow_internal(req.info, req.preferred_worker_id);
    }

    std::vector<WorkflowDeployRequest> failed_worker_requests;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto now = now_ms();
      auto failed_workers = find_unhealthy_workers_locked(now);
      if(!failed_workers.empty()) {
        failed_worker_requests = collect_failed_worker_workflows_locked(failed_workers);
        for(const auto& worker_id : failed_workers) {
          workers_.erase(worker_id);
        }
        for(auto it = workflow_to_worker_.begin(); it != workflow_to_worker_.end();) {
          if(std::find(failed_workers.begin(), failed_workers.end(), it->second) != failed_workers.end()) {
            it = workflow_to_worker_.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    for(const auto& req : failed_worker_requests) {
      deploy_workflow_internal(req.info, "");
    }
  }
}

std::vector<WorkerSnapshot> Deployer::list_worker_snapshots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WorkerSnapshot> out;
  out.reserve(workers_.size());
  for(const auto& kv : workers_) {
    WorkerSnapshot s;
    s.worker_id = kv.first;
    s.status = kv.second->get_status();
    s.load = kv.second->get_load();
    s.last_heartbeat_ms = kv.second->get_last_heartbeat_ms();
    s.preconfigured = kv.second->is_preconfigured();
    out.push_back(s);
  }
  return out;
}

std::vector<std::string> Deployer::list_registered_workers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  ids.reserve(workers_.size());
  for(const auto& kv : workers_) {
    ids.push_back(kv.first);
  }
  return ids;
}

int64_t Deployer::get_mapping_version() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mapping_version_;
}

}  // namespace master
}  // namespace runtime
}  // namespace agentflow
