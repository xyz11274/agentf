#ifndef AGENTFLOW_RUNTIME_MASTER_DEPLOYER_H
#define AGENTFLOW_RUNTIME_MASTER_DEPLOYER_H

#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "runtime/worker.h"
#include "runtime/shared_runtime_state.h"

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
  struct Config {
    int         schedule_interval_ms = 200;
    int64_t     heartbeat_timeout_ms = 10000;
    std::string plugin_so_path;
  };

  using DeployerAlgorithm = std::function<std::map<std::string, std::string>(
      const std::vector<WorkflowInstanceInfo>&, const std::vector<WorkerSnapshot>&,
      const std::map<std::string, std::string>&)>;

  Deployer();
  explicit Deployer(const Config& config);
  ~Deployer();

  bool register_worker(const std::shared_ptr<WorkerEngine>& worker);
  bool unregister_worker(const std::string& worker_id);

  bool update_worker_heartbeat(const std::string& worker_id);
  bool update_worker_status(const std::string& worker_id, WorkerEngine::WorkerStatus status);
  bool update_worker_load(const std::string& worker_id, int load);

  bool deploy_workflow_instance(const std::string& workflow_instance_id, const std::string& workflow_id,
                                const std::string& preferred_worker_id = "");
  bool enqueue_deploy_request(const WorkflowInstanceInfo& info,
                              const std::string& preferred_worker_id = "");
  bool remove_workflow_instance(const std::string& workflow_instance_id);
  bool get_workflow_worker(const std::string& workflow_instance_id, std::string& worker_id) const;
  std::map<std::string, std::string> get_all_deployments() const;
  bool                                rebalance(const std::vector<WorkflowInstanceInfo>& infos);
  bool                                handle_agent_state_change(const std::string& agent_instance_id,
                                                                bool is_healthy);

  std::vector<WorkerSnapshot> list_worker_snapshots() const;
  std::vector<std::string>    list_registered_workers() const;
  int64_t                     get_mapping_version() const;
  bool                        start();
  void                        stop();
  bool                        load_plugin_algorithm(const std::string& so_path);
  bool                        is_running() const { return running_; }

private:
  struct WorkerRegistrationRequest {
    std::shared_ptr<WorkerEngine> worker;
    bool                          unregister = false;
  };

  struct WorkflowDeployRequest {
    WorkflowInstanceInfo info;
    std::string          preferred_worker_id;
  };

  bool register_worker_internal(const std::shared_ptr<WorkerEngine>& worker);
  bool unregister_worker_internal(const std::string& worker_id);
  bool deploy_workflow_internal(const WorkflowInstanceInfo& info,
                                const std::string& preferred_worker_id);
  std::shared_ptr<WorkerEngine> pick_worker_for_workflow(const std::string& workflow_id,
                                                          const std::string& preferred_worker_id) const;
  std::vector<std::string>      find_unhealthy_workers_locked(int64_t now_ms) const;
  std::vector<WorkflowDeployRequest> collect_failed_worker_workflows_locked(
      const std::vector<std::string>& failed_workers) const;
  std::map<std::string, std::string> run_default_algorithm(
      const std::vector<WorkflowInstanceInfo>& infos, const std::vector<WorkerSnapshot>& workers,
      const std::map<std::string, std::string>& stable_mapping) const;
  static int64_t now_ms();
  void           registration_loop();
  void           scheduling_loop();

  Config                                              config_;
  mutable std::mutex                                  mutex_;
  std::map<std::string, std::shared_ptr<WorkerEngine>> workers_;
  std::map<std::string, std::string>                   workflow_to_worker_;
  int64_t                                              mapping_version_ = 0;

  std::atomic<bool> running_{false};
  std::thread       registration_thread_;
  std::thread       scheduling_thread_;
  std::queue<WorkerRegistrationRequest> worker_registration_queue_;
  std::queue<WorkflowDeployRequest>     workflow_deploy_queue_;
  std::mutex                            queue_mutex_;
  std::condition_variable               queue_cv_;

  DeployerAlgorithm algorithm_;
  void*             plugin_handle_ = nullptr;
};

}  // namespace master
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_MASTER_DEPLOYER_H
