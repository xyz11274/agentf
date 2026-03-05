// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Actor system manager implementation

#include "vendor/actor/actor_system.h"
#include "runtime/runtime_actor.h"
#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR
#include "protoactor/actor_system.h"
#endif

namespace agentflow {

ActorSystemManager& ActorSystemManager::get_instance() {
  static ActorSystemManager instance;
  return instance;
}

#ifdef AGENTFLOW_WITH_PROTOACTOR

bool ActorSystemManager::initialize() {
  std::lock_guard<std::mutex> lock(mutex_);

  if(initialized_) {
    return true;
  }

  try {
    // Create Protoactor actor system
    actor_system_ = protoactor::ActorSystem::New();

    if(!actor_system_) {
      return false;
    }

    initialized_ = true;
    return true;
  } catch(const std::exception& e) {
    return false;
  }
}

void ActorSystemManager::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);

  if(actor_system_) {
    actor_system_->Shutdown();
    actor_system_.reset();
  }

  initialized_ = false;
}

bool ActorSystemManager::is_running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_ && actor_system_ != nullptr;
}

std::shared_ptr<protoactor::ActorSystem> ActorSystemManager::get_actor_system() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return actor_system_;
}

ActorSystemManager::~ActorSystemManager() {
  shutdown();
}

#else

bool ActorSystemManager::initialize() {
  return false;
}

void ActorSystemManager::shutdown() {
}

bool ActorSystemManager::is_running() const {
  return false;
}

std::shared_ptr<void> ActorSystemManager::get_actor_system() const {
  return nullptr;
}

ActorSystemManager::~ActorSystemManager() {
}

#endif

}  // namespace agentflow

namespace agentflow {
namespace runtime {

MMRuntimeActor::MMRuntimeActor(std::shared_ptr<MMAgentRuntime> runtime) : runtime_(std::move(runtime)) {
}

void MMRuntimeActor::on_start() {
  Logger::instance().info("MMRuntimeActor started");
}

void MMRuntimeActor::on_stop() {
  Logger::instance().info("MMRuntimeActor stopped");
}

void MMRuntimeActor::on_message(std::shared_ptr<actor::adapter::IContext> ctx) {
  if(!ctx) {
    return;
  }
  handle_message(ctx->current_message());
}

bool MMRuntimeActor::handle_message(const actor::adapter::ActorMessage& msg) {
  if(!runtime_) {
    return false;
  }

  if(msg.message_type == "agent.register") {
    auto report = msg.get_payload<AgentRuntimeReport>();
    if(!report || !report->agent) {
      return false;
    }
    bool ok = runtime_->register_agent(report->agent);
    if(ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      agent_states_[report->agent_instance_id] = "registered";
    }
    return ok;
  }
  if(msg.message_type == "agent.heartbeat") {
    auto report = msg.get_payload<AgentRuntimeReport>();
    if(!report) {
      return false;
    }
    return runtime_->report_agent_heartbeat(report->agent_instance_id);
  }
  if(msg.message_type == "agent.status") {
    auto report = msg.get_payload<AgentRuntimeReport>();
    if(!report) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agent_states_[report->agent_instance_id] = std::to_string(static_cast<int>(report->state));
    return true;
  }

  if(msg.message_type == "task.status" || msg.message_type == "task.heartbeat"
     || msg.message_type == "task.register") {
    auto report = msg.get_payload<TaskRuntimeReport>();
    if(!report) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_states_[report->task_id] = report->status;
    }
    return runtime_->update_task_status(report->task_id, report->status, report->progress);
  }

  if(msg.message_type == "worker.register") {
    auto report = msg.get_payload<WorkerRuntimeReport>();
    if(!report || !report->worker) {
      return false;
    }
    bool ok = runtime_->register_worker_engine(report->worker);
    if(ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      worker_states_[report->worker_id] = "registered";
    }
    return ok;
  }
  if(msg.message_type == "worker.heartbeat") {
    auto report = msg.get_payload<WorkerRuntimeReport>();
    if(!report) {
      return false;
    }
    return runtime_->heartbeat_worker(report->worker_id);
  }
  if(msg.message_type == "worker.status") {
    auto report = msg.get_payload<WorkerRuntimeReport>();
    if(!report) {
      return false;
    }
    bool ok = runtime_->update_worker_engine_status(report->worker_id, report->status);
    if(ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      worker_states_[report->worker_id] = std::to_string(static_cast<int>(report->status));
    }
    return ok;
  }

  return false;
}

}  // namespace runtime
}  // namespace agentflow
