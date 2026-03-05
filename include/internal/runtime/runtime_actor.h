#ifndef AGENTFLOW_RUNTIME_RUNTIME_ACTOR_H
#define AGENTFLOW_RUNTIME_RUNTIME_ACTOR_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "agent/agent.h"
#include "runtime/agent_runtime.h"
#include "runtime/worker.h"
#include "vendor/actor/adapter.h"

namespace agentflow {
namespace runtime {

struct AgentRuntimeReport {
  std::string                      agent_instance_id;
  AgentState                       state = AgentState::CREATED;
  std::shared_ptr<framework::MMAgent> agent;
};

struct TaskRuntimeReport {
  std::string task_id;
  std::string status;
  int         progress = 0;
};

struct WorkerRuntimeReport {
  std::string                 worker_id;
  runtime::MMWorkerEngine::WorkerStatus status = runtime::MMWorkerEngine::WorkerStatus::STARTING;
  std::shared_ptr<MMWorkerEngine> worker;
};

class MMRuntimeActor : public actor::adapter::IActor {
public:
  explicit MMRuntimeActor(std::shared_ptr<MMAgentRuntime> runtime);
  ~MMRuntimeActor() override = default;

  void on_message(std::shared_ptr<actor::adapter::IContext> ctx) override;
  void on_start() override;
  void on_stop() override;
  std::string actor_type() const override { return "MMRuntimeActor"; }

  bool handle_message(const actor::adapter::ActorMessage& msg);
  std::shared_ptr<MMAgentRuntime> get_runtime() const { return runtime_; }

private:
  std::shared_ptr<MMAgentRuntime>             runtime_;
  mutable std::mutex                        mutex_;
  std::unordered_map<std::string, std::string> agent_states_;
  std::unordered_map<std::string, std::string> task_states_;
  std::unordered_map<std::string, std::string> worker_states_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_RUNTIME_ACTOR_H
