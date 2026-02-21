#ifndef AGENTFLOW_RUNTIME_WORKER_H
#define AGENTFLOW_RUNTIME_WORKER_H

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace agentflow {
namespace runtime {

class WorkerEngine {
public:
  enum class WorkerStatus {
    STARTING = 0,
    RUNNING  = 1,
    DEGRADED = 2,
    STOPPED  = 3,
  };

  struct Config {
    std::string worker_id;
    bool        preconfigured = false;
  };

  explicit WorkerEngine(const Config& config);
  ~WorkerEngine() = default;

  const std::string& get_worker_id() const { return config_.worker_id; }
  WorkerStatus       get_status() const;
  int                get_load() const;
  int64_t            get_last_heartbeat_ms() const;
  bool               is_preconfigured() const { return config_.preconfigured; }

  void set_status(WorkerStatus status);
  void set_load(int load);
  void heartbeat();

  void                     set_supported_workflows(const std::set<std::string>& workflow_ids);
  void                     add_supported_workflow(const std::string& workflow_id);
  void                     remove_supported_workflow(const std::string& workflow_id);
  bool                     supports_workflow(const std::string& workflow_id) const;
  std::set<std::string>    get_supported_workflows() const;

private:
  static int64_t now_ms();

  Config                 config_;
  WorkerStatus           status_ = WorkerStatus::STARTING;
  int                    load_ = 0;
  int64_t                last_heartbeat_ms_ = 0;
  std::set<std::string>  supported_workflows_;
  mutable std::mutex     mutex_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_WORKER_H
