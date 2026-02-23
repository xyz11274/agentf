#ifndef AGENTFLOW_RUNTIME_WORKER_WORKER_ACTOR_H
#define AGENTFLOW_RUNTIME_WORKER_WORKER_ACTOR_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "runtime/runtime_actor.h"
#include "runtime/worker.h"
#include "vendor/actor/adapter.h"

namespace agentflow {
namespace runtime {

class WorkerActor : public actor::adapter::IActor {
public:
  WorkerActor(std::shared_ptr<WorkerEngine> worker, std::shared_ptr<RuntimeActor> runtime_actor,
              int heartbeat_interval_ms = 1000);
  ~WorkerActor() override;

  void on_message(std::shared_ptr<actor::adapter::IContext> ctx) override;
  void on_start() override;
  void on_stop() override;
  std::string actor_type() const override { return "WorkerActor"; }

private:
  void heartbeat_loop();
  void report_register();
  void report_heartbeat();
  void report_status();

  std::shared_ptr<WorkerEngine> worker_;
  std::shared_ptr<RuntimeActor> runtime_actor_;
  int                           heartbeat_interval_ms_ = 1000;
  std::atomic<bool>             running_{false};
  std::thread                   heartbeat_thread_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_WORKER_WORKER_ACTOR_H
