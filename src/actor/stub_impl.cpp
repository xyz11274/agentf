// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Stub actor system implementation

#ifndef AGENTFLOW_WITH_PROTOACTOR

#include "agentflow/actor/adapter/stub_impl.h"

#include <chrono>
#include <sstream>

namespace agentflow {
namespace actor {
namespace adapter {

// StubActorRef implementation

StubActorRef::StubActorRef(const std::string& address) : address_(address) {
}

StubActorRef::~StubActorRef() = default;

std::string StubActorRef::address() const {
  return address_;
}

bool StubActorRef::is_alive() const {
  return alive_;
}

std::string StubActorRef::to_string() const {
  return address_;
}

// StubContext implementation

StubContext::StubContext() = default;

StubContext::~StubContext() = default;

std::shared_ptr<IActorRef> StubContext::self() {
  return self_;
}

std::shared_ptr<IActorRef> StubContext::parent() {
  return parent_;
}

std::shared_ptr<IActorRef> StubContext::sender() {
  return sender_;
}

ActorMessage StubContext::current_message() const {
  return current_msg_;
}

void StubContext::send(IActorRef* target, const ActorMessage& msg) {
  // Stub implementation: just log
  // In real stub, would queue message for target actor
}

ActorMessage StubContext::ask(IActorRef* target, const ActorMessage& msg, int timeout_ms) {
  // Stub implementation: return empty message
  ActorMessage result;
  result.message_type = "reply";
  return result;
}

// StubActorSystem implementation

StubActorSystem::StubActorSystem() {
  std::stringstream ss;
  ss << "agentflow-stub-" << std::chrono::high_resolution_clock::now().time_since_epoch().count();
  system_id_ = ss.str();
}

StubActorSystem::~StubActorSystem() {
  shutdown();
}

std::shared_ptr<IActorRef> StubActorSystem::spawn(const ActorFactory& factory,
                                                  const std::string&  actor_name) {
  return spawn_with_config(factory, actor_name, 1000);
}

std::shared_ptr<IActorRef> StubActorSystem::spawn_with_config(const ActorFactory& factory,
                                                              const std::string&  actor_name,
                                                              int                 mailbox_size) {
  (void)mailbox_size;
  std::lock_guard<std::mutex> lock(actors_mutex_);

  if(!running_) {
    return nullptr;
  }

  auto actor_internal   = std::make_shared<StubActorInternal>();
  actor_internal->actor = factory();

  if(!actor_internal->actor) {
    return nullptr;
  }

  std::string address = actor_name;
  if(address.empty()) {
    std::stringstream ss;
    ss << "actor-" << (next_actor_id_++);
    address = ss.str();
  }

  actor_internal->ref = std::make_shared<StubActorRef>(address);

  // Start worker thread for this actor
  actor_internal->worker_thread =
      std::thread(&StubActorSystem::actor_worker_loop, this, actor_internal);

  actors_[address] = actor_internal;
  return actor_internal->ref;
}

std::shared_ptr<IActorRef> StubActorSystem::get_actor(const std::string& address) {
  std::lock_guard<std::mutex> lock(actors_mutex_);

  auto it = actors_.find(address);
  if(it != actors_.end()) {
    return it->second->ref;
  }

  return nullptr;
}

void StubActorSystem::stop(std::shared_ptr<IActorRef> actor_ref) {
  if(!actor_ref) {
    return;
  }

  std::lock_guard<std::mutex> lock(actors_mutex_);

  auto it = actors_.find(actor_ref->address());
  if(it != actors_.end()) {
    it->second->should_stop = true;
    it->second->mailbox_cv.notify_all();
  }
}

void StubActorSystem::shutdown() {
  std::lock_guard<std::mutex> lock(actors_mutex_);

  running_ = false;

  // Signal all actors to stop
  for(auto& pair : actors_) {
    pair.second->should_stop = true;
    pair.second->mailbox_cv.notify_all();
  }

  // Wait for all worker threads to finish
  for(auto& pair : actors_) {
    if(pair.second->worker_thread.joinable()) {
      pair.second->worker_thread.join();
    }
  }

  actors_.clear();
}

bool StubActorSystem::is_running() const {
  std::lock_guard<std::mutex> lock(actors_mutex_);
  return running_;
}

std::string StubActorSystem::system_address() const {
  return system_id_;
}

bool StubActorSystem::wait_shutdown(int timeout_ms) {
  auto start = std::chrono::high_resolution_clock::now();

  while(is_running()) {
    if(timeout_ms > 0) {
      auto now     = std::chrono::high_resolution_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

      if(elapsed > timeout_ms) {
        return false;  // Timeout
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return true;  // Shutdown completed
}

void StubActorSystem::actor_worker_loop(std::shared_ptr<StubActorInternal> actor_internal) {
  auto context = std::make_shared<StubContext>();
  context->set_self(actor_internal->ref);

  // Call on_start
  if(actor_internal->actor) {
    actor_internal->actor->on_start();
  }

  // Process messages until stop signal
  while(!actor_internal->should_stop) {
    std::unique_lock<std::mutex> lock(actor_internal->mailbox_mutex);

    // Wait for message or stop signal
    actor_internal->mailbox_cv.wait_for(lock, std::chrono::milliseconds(100), [&actor_internal] {
      return !actor_internal->mailbox.empty() || actor_internal->should_stop;
    });

    if(!actor_internal->mailbox.empty()) {
      ActorMessage msg = actor_internal->mailbox.front();
      actor_internal->mailbox.pop();

      context->set_current_message(msg);

      lock.unlock();

      // Process message
      if(actor_internal->actor) {
        try {
          actor_internal->actor->on_message(context);
        } catch(const std::exception&) {
          // Handle exception
        }
      }
    }
  }

  // Call on_stop
  if(actor_internal->actor) {
    actor_internal->actor->on_stop();
  }
}

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
