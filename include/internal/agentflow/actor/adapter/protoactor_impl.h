// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Protoactor-cpp implementation of the actor adapter

#ifndef AGENTFLOW_ACTOR_ADAPTER_PROTOACTOR_IMPL_H
#define AGENTFLOW_ACTOR_ADAPTER_PROTOACTOR_IMPL_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <memory>
#include <string>

#include "agentflow/actor/adapter.h"

// Forward declare Protoactor types
namespace protoactor {
class PID;
class Context;
class Actor;
class ActorSystem;
}  // namespace protoactor

namespace agentflow {
namespace actor {
namespace adapter {

/// @brief Protoactor implementation of IActorRef
class ProtoactorActorRef : public IActorRef {
public:
  explicit ProtoactorActorRef(std::shared_ptr<protoactor::PID> pid);
  ~ProtoactorActorRef() override;

  std::string address() const override;
  bool        is_alive() const override;
  std::string to_string() const override;

  /// @brief Get underlying Protoactor PID
  std::shared_ptr<protoactor::PID> get_pid() const;

private:
  std::shared_ptr<protoactor::PID> pid_;
};

/// @brief Protoactor implementation of IContext
class ProtoactorContext : public IContext {
public:
  explicit ProtoactorContext(std::shared_ptr<protoactor::Context> ctx);
  ~ProtoactorContext() override;

  std::shared_ptr<IActorRef> self() override;
  std::shared_ptr<IActorRef> parent() override;
  std::shared_ptr<IActorRef> sender() override;
  ActorMessage               current_message() const override;
  void                       send(IActorRef* target, const ActorMessage& msg) override;
  ActorMessage ask(IActorRef* target, const ActorMessage& msg, int timeout_ms) override;

private:
  std::shared_ptr<protoactor::Context> ctx_;
  mutable ActorMessage                 current_msg_;
};

/// @brief Bridge from IActor to protoactor::Actor
/// Wraps an IActor to work with Protoactor framework
class ProtoactorBridge : public protoactor::Actor {
public:
  explicit ProtoactorBridge(std::shared_ptr<IActor> actor);
  ~ProtoactorBridge() override;

  /// @brief Called by Protoactor runtime
  void Receive(std::shared_ptr<protoactor::Context> context) override;

private:
  std::shared_ptr<IActor> actor_;
};

/// @brief Protoactor implementation of IActorSystem
class ProtoactorActorSystem : public IActorSystem {
public:
  explicit ProtoactorActorSystem(std::shared_ptr<protoactor::ActorSystem> system);
  ~ProtoactorActorSystem() override;

  std::shared_ptr<IActorRef> spawn(const ActorFactory& factory,
                                   const std::string&  actor_name = "") override;

  std::shared_ptr<IActorRef> spawn_with_config(const ActorFactory& factory,
                                               const std::string&  actor_name,
                                               int                 mailbox_size) override;

  std::shared_ptr<IActorRef> get_actor(const std::string& address) override;
  void                       stop(std::shared_ptr<IActorRef> actor_ref) override;
  void                       shutdown() override;
  bool                       is_running() const override;
  std::string                system_address() const override;
  bool                       wait_shutdown(int timeout_ms = 0) override;

private:
  std::shared_ptr<protoactor::ActorSystem> system_;
  mutable std::mutex                       mutex_;
  bool                                     shutdown_called_ = false;
};

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_ADAPTER_PROTOACTOR_IMPL_H
