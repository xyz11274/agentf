#ifndef AGENTFLOW_RUNTIME_MASTER_DEPLOYER_H
#define AGENTFLOW_RUNTIME_MASTER_DEPLOYER_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/worker.h"

namespace agentflow {
namespace runtime {
namespace master {

struct WorkerSnapshot {
  std::string             worker_id;
  WorkerEngine::WorkerStatus status = WorkerEngine::WorkerStatus::STARTING;
  int                     load = 0;
  int64_t                 last_heartbeat_ms = 0;
  bool                    preconfigured = false;
};

class Deployer {
public:
  Deployer() = default;
  ~Deployer() = default;

  bool register_worker(const std::shared_ptr<WorkerEngine>& worker);
  bool unregister_worker(const std::string& worker_id);

  bool update_worker_heartbeat(const std::string& worker_id);
  bool update_worker_status(const std::string& worker_id, WorkerEngine::WorkerStatus status);
  bool update_worker_load(const std::string& worker_id, int load);

  bool deploy_workflow_instance(const std::string& workflow_instance_id, const std::string& workflow_id,
                                const std::string& preferred_worker_id = "");
  bool remove_workflow_instance(const std::string& workflow_instance_id);
  bool get_workflow_worker(const std::string& workflow_instance_id, std::string& worker_id) const;

  std::vector<WorkerSnapshot> list_worker_snapshots() const;
  std::vector<std::string>    list_registered_workers() const;
  int64_t                     get_mapping_version() const;

private:
  std::shared_ptr<WorkerEngine> pick_worker_for_workflow(const std::string& workflow_id,
                                                          const std::string& preferred_worker_id) const;

  mutable std::mutex                                  mutex_;
  std::map<std::string, std::shared_ptr<WorkerEngine>> workers_;
  std::map<std::string, std::string>                   workflow_to_worker_;
  int64_t                                              mapping_version_ = 0;
};

}  // namespace master
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_MASTER_DEPLOYER_H
