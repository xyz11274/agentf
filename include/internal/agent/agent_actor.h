// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#ifndef AGENTFLOW_AGENT_ACTOR_H
#define AGENTFLOW_AGENT_ACTOR_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "runtime/runtime_actor.h"
#include "vendor/actor/adapter.h"

namespace agentflow {
namespace agent {

class AgentActor : public actor::adapter::IActor {
public:
  struct ServiceConfig {
    std::string              package_root = ".";
    std::vector<std::string> manifests;
    int                      scan_interval_ms = 1000;
    int                      heartbeat_interval_ms = 1000;
  };

  explicit AgentActor(std::shared_ptr<framework::Agent> agent);
  explicit AgentActor(const std::string& config_path);
  AgentActor(const std::string& config_path, std::shared_ptr<runtime::RuntimeActor> runtime_actor);
  ~AgentActor() override;

  void on_message(std::shared_ptr<actor::adapter::IContext> ctx) override;
  void on_start() override;
  void on_stop() override;

  std::string actor_type() const override { return "AgentActor"; }

private:
  struct ManagedAgent {
    std::string                      instance_id;
    std::string                      package_id;
    std::string                      version;
    std::string                      package_path;
    std::shared_ptr<framework::Agent> agent;
    int64_t                          last_mtime_ms = 0;
  };

  bool parse_service_config();
  void load_agents_from_config();
  void monitor_packages_loop();
  void heartbeat_loop();
  void collect_agent_tasks_and_report();

  bool create_agent_from_package(const std::string& package_path);
  bool should_reload(const std::string& package_path, int64_t& out_mtime_ms) const;
  void apply_agent_control(const std::string& action, const std::string& instance_id);
  void apply_task_control(const std::string& action, const std::string& task_id);
  void report_agent_registered(const ManagedAgent& managed);
  void report_agent_heartbeat(const ManagedAgent& managed);
  void report_agent_status(const ManagedAgent& managed);
  void report_task_status(const std::string& task_id, const std::string& status, int progress);

  std::string                           config_path_;
  ServiceConfig                         service_config_;
  std::map<std::string, ManagedAgent>   managed_agents_;
  std::map<std::string, std::shared_ptr<framework::AgentTask>> managed_tasks_;
  std::shared_ptr<runtime::RuntimeActor> runtime_actor_;
  std::atomic<bool>                     running_{false};
  std::thread                           package_monitor_thread_;
  std::thread                           heartbeat_thread_;
  mutable std::mutex                    mutex_;
};

}  // namespace agent
}  // namespace agentflow

#endif  // AGENTFLOW_AGENT_ACTOR_H
