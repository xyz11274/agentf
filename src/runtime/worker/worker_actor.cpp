#include "runtime/worker/worker_actor.h"

#include <algorithm>
#include <chrono>

namespace agentflow {
namespace runtime {

WorkerActor::WorkerActor(std::shared_ptr<MMWorkerEngine> worker,
                         std::shared_ptr<MMRuntimeActor> runtime_actor, int heartbeat_interval_ms)
    : worker_(std::move(worker)),
      runtime_actor_(std::move(runtime_actor)),
      heartbeat_interval_ms_(heartbeat_interval_ms) {
}

WorkerActor::~WorkerActor() {
  on_stop();
}

void WorkerActor::report_register() {
  if(!worker_ || !runtime_actor_) {
    return;
  }
  WorkerRuntimeReport report;
  report.worker_id = worker_->get_worker_id();
  report.status = worker_->get_status();
  report.worker = worker_;
  actor::adapter::ActorMessage msg;
  msg.message_type = "worker.register";
  msg.payload = std::make_shared<WorkerRuntimeReport>(report);
  runtime_actor_->handle_message(msg);
}

void WorkerActor::report_heartbeat() {
  if(!worker_ || !runtime_actor_) {
    return;
  }
  WorkerRuntimeReport report;
  report.worker_id = worker_->get_worker_id();
  report.status = worker_->get_status();
  actor::adapter::ActorMessage msg;
  msg.message_type = "worker.heartbeat";
  msg.payload = std::make_shared<WorkerRuntimeReport>(report);
  runtime_actor_->handle_message(msg);
}

void WorkerActor::report_status() {
  if(!worker_ || !runtime_actor_) {
    return;
  }
  WorkerRuntimeReport report;
  report.worker_id = worker_->get_worker_id();
  report.status = worker_->get_status();
  actor::adapter::ActorMessage msg;
  msg.message_type = "worker.status";
  msg.payload = std::make_shared<WorkerRuntimeReport>(report);
  runtime_actor_->handle_message(msg);
}

void WorkerActor::heartbeat_loop() {
  while(running_) {
    report_heartbeat();
    report_status();
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(100, heartbeat_interval_ms_)));
  }
}

void WorkerActor::on_start() {
  if(running_.exchange(true)) {
    return;
  }
  if(worker_) {
    worker_->start();
  }
  report_register();
  report_status();
  heartbeat_thread_ = std::thread(&WorkerActor::heartbeat_loop, this);
}

void WorkerActor::on_stop() {
  if(!running_.exchange(false)) {
    return;
  }
  if(heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }
  if(worker_) {
    worker_->stop();
  }
  report_status();
}

void WorkerActor::on_message(std::shared_ptr<actor::adapter::IContext> ctx) {
  if(!ctx || !worker_) {
    return;
  }
  auto msg = ctx->current_message();
  if(msg.message_type == "worker.start") {
    worker_->set_status(MMWorkerEngine::WorkerStatus::RUNNING);
    report_status();
  } else if(msg.message_type == "worker.pause") {
    worker_->set_status(MMWorkerEngine::WorkerStatus::DEGRADED);
    report_status();
  } else if(msg.message_type == "worker.stop") {
    worker_->set_status(MMWorkerEngine::WorkerStatus::STOPPED);
    report_status();
  } else if(msg.message_type == "worker.sync_status") {
    report_status();
  }
}

}  // namespace runtime
}  // namespace agentflow
