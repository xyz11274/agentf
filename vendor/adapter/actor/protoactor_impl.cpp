// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Protoactor-cpp implementation of actor adapter.
// Aligned to https://github.com/DanielKernel/Protoactor-cpp API.

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include "vendor/actor/protoactor_impl.h"

#include <chrono>
#include <thread>

#include "protoactor/actor_system.h"
#include "protoactor/context.h"
#include "protoactor/future.h"
#include "protoactor/messages.h"
#include "protoactor/pid.h"
#include "protoactor/props.h"

namespace agentflow {
namespace actor {
namespace adapter {

// ============================================================================
// ProtoactorActorRef implementation
// ============================================================================

ProtoactorActorRef::ProtoactorActorRef(std::shared_ptr<protoactor::PID> pid) : pid_(pid) {}

ProtoactorActorRef::~ProtoactorActorRef() = default;

std::string ProtoactorActorRef::address() const {
  if(!pid_) {
    return "";
  }
  // PID exposes public members `address` (node address) and `id` (actor name)
  return pid_->address + "/" + pid_->id;
}

bool ProtoactorActorRef::is_alive() const {
  // PID itself has no stopped flag; liveness is inferred from system state
  return pid_ != nullptr;
}

std::string ProtoactorActorRef::to_string() const {
  return address();
}

std::shared_ptr<protoactor::PID> ProtoactorActorRef::get_pid() const {
  return pid_;
}

// ============================================================================
// ProtoactorContext implementation
// ============================================================================

ProtoactorContext::ProtoactorContext(std::shared_ptr<protoactor::Context> ctx) : ctx_(ctx) {}

ProtoactorContext::~ProtoactorContext() = default;

std::shared_ptr<IActorRef> ProtoactorContext::self() {
  if(!ctx_) {
    return nullptr;
  }
  return std::make_shared<ProtoactorActorRef>(ctx_->Self());
}

std::shared_ptr<IActorRef> ProtoactorContext::parent() {
  if(!ctx_) {
    return nullptr;
  }
  auto parent_pid = ctx_->Parent();
  if(!parent_pid) {
    return nullptr;
  }
  return std::make_shared<ProtoactorActorRef>(parent_pid);
}

std::shared_ptr<IActorRef> ProtoactorContext::sender() {
  if(!ctx_) {
    return nullptr;
  }
  auto sender_pid = ctx_->Sender();
  if(!sender_pid) {
    return nullptr;
  }
  return std::make_shared<ProtoactorActorRef>(sender_pid);
}

ActorMessage ProtoactorContext::current_message() const {
  return current_msg_;
}

void ProtoactorContext::send(IActorRef* target, const ActorMessage& msg) {
  if(!ctx_ || !target) {
    return;
  }

  auto actor_ref = dynamic_cast<ProtoactorActorRef*>(target);
  if(!actor_ref) {
    return;
  }

  auto target_pid = actor_ref->get_pid();
  if(!target_pid) {
    return;
  }

  // Wrap in an ActorMessage shared_ptr so the remote actor can unpack it
  ctx_->Send(target_pid, std::make_shared<ActorMessage>(msg));
}

ActorMessage ProtoactorContext::ask(IActorRef* target, const ActorMessage& msg, int timeout_ms) {
  ActorMessage error_result;
  error_result.message_type = "error";

  if(!ctx_ || !target) {
    return error_result;
  }

  auto actor_ref = dynamic_cast<ProtoactorActorRef*>(target);
  if(!actor_ref) {
    return error_result;
  }

  auto target_pid = actor_ref->get_pid();
  if(!target_pid) {
    return error_result;
  }

  // Context::RequestFuture sends msg and returns a future for the reply
  auto future = ctx_->RequestFuture(
      target_pid, std::make_shared<ActorMessage>(msg),
      std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000));

  // Future::Result() blocks until the reply arrives or the timeout expires
  auto [reply_payload, ec] = future->Result();

  if(ec) {
    ActorMessage timeout_result;
    timeout_result.message_type = ec == std::errc::timed_out ? "timeout" : "error";
    return timeout_result;
  }

  // The reply payload is expected to be an ActorMessage
  auto reply = std::static_pointer_cast<ActorMessage>(reply_payload);
  if(reply) {
    return *reply;
  }

  // Caller sent a raw payload – wrap it
  ActorMessage wrapped;
  wrapped.message_type = "response";
  wrapped.payload      = reply_payload;
  return wrapped;
}

// ============================================================================
// ProtoactorBridge implementation
// ============================================================================

ProtoactorBridge::ProtoactorBridge(std::shared_ptr<IActor> actor) : actor_(actor) {}

ProtoactorBridge::~ProtoactorBridge() = default;

void ProtoactorBridge::Receive(std::shared_ptr<protoactor::Context> context) {
  if(!context) {
    return;
  }

  auto adapter_context = std::make_shared<ProtoactorContext>(context);

  // Handle Protoactor lifecycle messages
  auto raw_msg = context->Message();
  if(raw_msg) {
    if(std::static_pointer_cast<protoactor::Started>(raw_msg)) {
      if(actor_) {
        actor_->on_start();
      }
      return;
    }
    if(std::static_pointer_cast<protoactor::Stopping>(raw_msg) ||
       std::static_pointer_cast<protoactor::Stopped>(raw_msg)) {
      if(actor_) {
        actor_->on_stop();
      }
      return;
    }
  }

  if(!actor_) {
    return;
  }

  // Unpack the AgentFlow ActorMessage envelope
  ActorMessage current_msg;
  auto         envelope = std::static_pointer_cast<ActorMessage>(raw_msg);
  if(envelope) {
    current_msg = *envelope;
  } else {
    // Raw payload from non-AgentFlow sender; wrap it transparently
    current_msg.message_type = "raw";
    current_msg.payload      = raw_msg;
  }
  adapter_context->current_msg_ = current_msg;

  try {
    actor_->on_message(adapter_context);
  } catch(const std::exception&) {
    // Exceptions in on_message are swallowed; actor isolation prevents cascading failures.
    // The Protoactor supervisor will decide whether to restart the actor.
  }
}

// ============================================================================
// ProtoactorActorSystem implementation
// ============================================================================

ProtoactorActorSystem::ProtoactorActorSystem(std::shared_ptr<protoactor::ActorSystem> system)
    : system_(system) {}

ProtoactorActorSystem::~ProtoactorActorSystem() {
  shutdown();
}

std::shared_ptr<IActorRef> ProtoactorActorSystem::spawn(const ActorFactory& factory,
                                                        const std::string&  actor_name) {
  return spawn_with_config(factory, actor_name, 0);
}

std::shared_ptr<IActorRef> ProtoactorActorSystem::spawn_with_config(const ActorFactory& factory,
                                                                    const std::string&  actor_name,
                                                                    int mailbox_size) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!system_ || system_->IsStopped()) {
      return nullptr;
    }
  }

  auto actor = factory();
  if(!actor) {
    return nullptr;
  }

  auto bridge = std::make_shared<ProtoactorBridge>(actor);

  // Props::FromProducer returns shared_ptr<Props>
  auto props = protoactor::Props::FromProducer(
      [bridge]() -> std::shared_ptr<protoactor::Actor> { return bridge; });

  if(mailbox_size > 0) {
    // Bounded mailbox via a producer lambda
    props = props->WithMailboxProducer(
        [mailbox_size]() { return protoactor::BoundedMailbox::New(mailbox_size); });
  }

  auto root = system_->GetRoot();
  if(!root) {
    return nullptr;
  }

  std::shared_ptr<protoactor::PID> pid;
  if(actor_name.empty()) {
    pid = root->Spawn(props);
  } else {
    // SpawnNamed returns pair<shared_ptr<PID>, error_code>
    auto [named_pid, ec] = root->SpawnNamed(props, actor_name);
    if(ec) {
      return nullptr;
    }
    pid = named_pid;
  }

  if(!pid) {
    return nullptr;
  }

  return std::make_shared<ProtoactorActorRef>(pid);
}

std::shared_ptr<IActorRef> ProtoactorActorSystem::get_actor(const std::string& address) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!system_ || system_->IsStopped()) {
      return nullptr;
    }
  }

  // address format is "<node_address>/<actor_id>" – extract the actor id portion
  std::string actor_id = address;
  auto        slash    = address.rfind('/');
  if(slash != std::string::npos) {
    actor_id = address.substr(slash + 1);
  }

  // Look up a local process by id through the process registry
  auto process = system_->GetProcessRegistry()->GetLocal(actor_id);
  if(!process) {
    return nullptr;
  }

  // Re-construct a PID from the address components
  auto pid     = std::make_shared<protoactor::PID>();
  pid->address = (slash != std::string::npos) ? address.substr(0, slash) : system_->Address();
  pid->id      = actor_id;
  return std::make_shared<ProtoactorActorRef>(pid);
}

void ProtoactorActorSystem::stop(std::shared_ptr<IActorRef> actor_ref) {
  if(!actor_ref) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!system_ || system_->IsStopped()) {
      return;
    }
  }

  auto protoactor_ref = dynamic_cast<ProtoactorActorRef*>(actor_ref.get());
  if(!protoactor_ref) {
    return;
  }

  auto pid = protoactor_ref->get_pid();
  if(!pid) {
    return;
  }

  auto root = system_->GetRoot();
  if(root) {
    root->Stop(pid);
  }
}

void ProtoactorActorSystem::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);

  if(!system_ || system_->IsStopped()) {
    return;
  }

  system_->Shutdown();
}

bool ProtoactorActorSystem::is_running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return system_ != nullptr && !system_->IsStopped();
}

std::string ProtoactorActorSystem::system_address() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(!system_) {
    return "";
  }
  return system_->Address();
}

bool ProtoactorActorSystem::wait_shutdown(int timeout_ms) {
  auto start = std::chrono::high_resolution_clock::now();

  while(is_running()) {
    if(timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - start)
                         .count();
      if(elapsed > timeout_ms) {
        return false;
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
