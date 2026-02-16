// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Protoactor-cpp implementation of actor adapter

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include "agentflow/actor/adapter/protoactor_impl.h"
#include "protoactor/actor_system.h"
#include "protoactor/context.h"
#include "protoactor/pid.h"
#include "protoactor/props.h"
#include <thread>

namespace agentflow {
namespace actor {
namespace adapter {

// ProtoactorActorRef implementation

ProtoactorActorRef::ProtoactorActorRef(std::shared_ptr<protoactor::PID> pid)
    : pid_(pid) {}

ProtoactorActorRef::~ProtoactorActorRef() = default;

std::string ProtoactorActorRef::address() const {
  if (!pid_) {
    return "";
  }
  return pid_->ToString();
}

bool ProtoactorActorRef::is_alive() const {
  // TODO: Implement proper liveness check
  return pid_ != nullptr;
}

std::string ProtoactorActorRef::to_string() const {
  return address();
}

std::shared_ptr<protoactor::PID> ProtoactorActorRef::get_pid() const {
  return pid_;
}

// ProtoactorContext implementation

ProtoactorContext::ProtoactorContext(
    std::shared_ptr<protoactor::Context> ctx)
    : ctx_(ctx) {}

ProtoactorContext::~ProtoactorContext() = default;

std::shared_ptr<IActorRef> ProtoactorContext::self() {
  if (!ctx_) {
    return nullptr;
  }
  
  auto self_pid = ctx_->Self();
  return std::make_shared<ProtoactorActorRef>(self_pid);
}

std::shared_ptr<IActorRef> ProtoactorContext::parent() {
  if (!ctx_) {
    return nullptr;
  }
  
  auto parent_pid = ctx_->Parent();
  if (!parent_pid) {
    return nullptr;
  }
  
  return std::make_shared<ProtoactorActorRef>(parent_pid);
}

std::shared_ptr<IActorRef> ProtoactorContext::sender() {
  if (!ctx_) {
    return nullptr;
  }
  
  auto sender_pid = ctx_->Sender();
  if (!sender_pid) {
    return nullptr;
  }
  
  return std::make_shared<ProtoactorActorRef>(sender_pid);
}

ActorMessage ProtoactorContext::current_message() const {
  return current_msg_;
}

void ProtoactorContext::send(IActorRef* target, const ActorMessage& msg) {
  if (!ctx_ || !target) {
    return;
  }

  auto actor_ref = dynamic_cast<ProtoactorActorRef*>(target);
  if (!actor_ref) {
    return;
  }

  auto target_pid = actor_ref->get_pid();
  if (!target_pid) {
    return;
  }

  // Convert ActorMessage to Protoactor message
  // This is a simplified version - real implementation would need
  // proper message serialization
  ctx_->Send(target_pid, msg.payload);
}

ActorMessage ProtoactorContext::ask(IActorRef* target,
                                    const ActorMessage& msg,
                                    int timeout_ms) {
  ActorMessage result;
  
  if (!ctx_ || !target) {
    return result;
  }

  auto actor_ref = dynamic_cast<ProtoactorActorRef*>(target);
  if (!actor_ref) {
    return result;
  }

  auto target_pid = actor_ref->get_pid();
  if (!target_pid) {
    return result;
  }

  try {
    // Request-reply pattern using Protoactor
    // TODO: Implement proper ask pattern with timeout
    result.message_type = "timeout";
  } catch (const std::exception&) {
    result.message_type = "error";
  }

  return result;
}

// ProtoactorBridge implementation

ProtoactorBridge::ProtoactorBridge(std::shared_ptr<IActor> actor)
    : actor_(actor) {}

ProtoactorBridge::~ProtoactorBridge() = default;

void ProtoactorBridge::Receive(std::shared_ptr<protoactor::Context> context) {
  if (!actor_ || !context) {
    return;
  }

  auto adapter_context = std::make_shared<ProtoactorContext>(context);
  
  try {
    actor_->on_message(adapter_context);
  } catch (const std::exception&) {
    // Handle exception - could stop actor or log error
  }
}

// ProtoactorActorSystem implementation

ProtoactorActorSystem::ProtoactorActorSystem(
    std::shared_ptr<protoactor::ActorSystem> system)
    : system_(system) {}

ProtoactorActorSystem::~ProtoactorActorSystem() {
  shutdown();
}

std::shared_ptr<IActorRef> ProtoactorActorSystem::spawn(
    const ActorFactory& factory,
    const std::string& actor_name) {
  return spawn_with_config(factory, actor_name, 0);
}

std::shared_ptr<IActorRef> ProtoactorActorSystem::spawn_with_config(
    const ActorFactory& factory,
    const std::string& actor_name,
    int mailbox_size) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!system_ || shutdown_called_) {
      return nullptr;
    }
  }

  try {
    // Create the IActor instance
    auto actor = factory();
    if (!actor) {
      return nullptr;
    }

    // Wrap it in ProtoactorBridge
    auto bridge = std::make_shared<ProtoactorBridge>(actor);

    // Create props for the bridge
    auto props = protoactor::Props::FromProducer(
        [bridge]() -> std::shared_ptr<protoactor::Actor> {
          return bridge;
        });

    // TODO: Apply mailbox_size configuration if needed

    // Spawn the actor
    auto root = system_->GetRoot();
    if (!root) {
      return nullptr;
    }

    std::shared_ptr<protoactor::PID> pid;
    if (actor_name.empty()) {
      pid = root->Spawn(props);
    } else {
      // Named spawn - implementation depends on Protoactor version
      pid = root->Spawn(props);  // TODO: Support named actors
    }

    if (!pid) {
      return nullptr;
    }

    return std::make_shared<ProtoactorActorRef>(pid);
  } catch (const std::exception&) {
    return nullptr;
  }
}

std::shared_ptr<IActorRef> ProtoactorActorSystem::get_actor(
    const std::string& address) {
  // TODO: Implement actor lookup by address
  return nullptr;
}

void ProtoactorActorSystem::stop(std::shared_ptr<IActorRef> actor_ref) {
  if (!actor_ref) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!system_) {
      return;
    }
  }

  auto protoactor_ref = dynamic_cast<ProtoactorActorRef*>(actor_ref.get());
  if (!protoactor_ref) {
    return;
  }

  auto pid = protoactor_ref->get_pid();
  if (!pid) {
    return;
  }

  auto root = system_->GetRoot();
  if (root) {
    root->Stop(pid);
  }
}

void ProtoactorActorSystem::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (shutdown_called_) {
    return;
  }

  shutdown_called_ = true;

  if (system_) {
    system_->Shutdown();
    // Wait a bit for shutdown to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

bool ProtoactorActorSystem::is_running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return system_ != nullptr && !shutdown_called_;
}

std::string ProtoactorActorSystem::system_address() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!system_) {
    return "";
  }
  return system_->Address();
}

bool ProtoactorActorSystem::wait_shutdown(int timeout_ms) {
  auto start = std::chrono::high_resolution_clock::now();

  while (is_running()) {
    if (timeout_ms > 0) {
      auto now = std::chrono::high_resolution_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - start).count();
      
      if (elapsed > timeout_ms) {
        return false;  // Timeout
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return true;
}

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
