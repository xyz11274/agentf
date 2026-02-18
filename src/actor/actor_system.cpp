// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Actor system manager implementation

#include "actor/actor_system.h"

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
