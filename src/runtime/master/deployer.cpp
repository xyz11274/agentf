#include "runtime/master/deployer.h"

namespace agentflow {
namespace runtime {
namespace master {

bool Deployer::register_worker(const std::shared_ptr<WorkerEngine>& worker) {
  if(!worker) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  workers_[worker->get_worker_id()] = worker;
  ++mapping_version_;
  return true;
}

bool Deployer::unregister_worker(const std::string& worker_id) {
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

bool Deployer::update_worker_heartbeat(const std::string& worker_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = workers_.find(worker_id);
  if(it == workers_.end()) {
    return false;
  }
  it->second->heartbeat();
  return true;
}

bool Deployer::update_worker_status(const std::string& worker_id, WorkerEngine::WorkerStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = workers_.find(worker_id);
  if(it == workers_.end()) {
    return false;
  }
  it->second->set_status(status);
  return true;
}

bool Deployer::update_worker_load(const std::string& worker_id, int load) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = workers_.find(worker_id);
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
       && it->second->get_status() != WorkerEngine::WorkerStatus::STOPPED) {
      return it->second;
    }
  }

  std::shared_ptr<WorkerEngine> picked = nullptr;
  int                           min_load = 0;
  for(const auto& kv : workers_) {
    const auto& worker = kv.second;
    if(worker->get_status() == WorkerEngine::WorkerStatus::STOPPED
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

bool Deployer::deploy_workflow_instance(const std::string& workflow_instance_id,
                                        const std::string& workflow_id,
                                        const std::string& preferred_worker_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        worker = pick_worker_for_workflow(workflow_id, preferred_worker_id);
  if(!worker) {
    return false;
  }
  workflow_to_worker_[workflow_instance_id] = worker->get_worker_id();
  ++mapping_version_;
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
  auto                        it = workflow_to_worker_.find(workflow_instance_id);
  if(it == workflow_to_worker_.end()) {
    return false;
  }
  worker_id = it->second;
  return true;
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
  std::vector<std::string>    ids;
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
