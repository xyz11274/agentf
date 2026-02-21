// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Stub implementation of the actor adapter (no Protoactor required)
// Used for testing and when Protoactor-cpp is not available

#ifndef AGENTFLOW_ACTOR_ADAPTER_STUB_IMPL_H
#define AGENTFLOW_ACTOR_ADAPTER_STUB_IMPL_H

#ifndef AGENTFLOW_WITH_PROTOACTOR

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "vendor/actor/adapter.h"

namespace agentflow {
namespace actor {
namespace adapter {

// Forward declarations
class StubActorSystem;
class StubContext;

/// @brief Stub implementation of IActorRef
class StubActorRef : public IActorRef {
public:
  explicit StubActorRef(const std::string& address);
  ~StubActorRef() override;

  std::string address() const override;
  bool        is_alive() const override;
  std::string to_string() const override;

  void set_alive(bool alive) { alive_ = alive; }

private:
  std::string address_;
  bool        alive_ = true;
};

/// @brief Pending reply-future for ask pattern
struct StubReplyFuture {
  ActorMessage            reply;
  std::mutex              mutex;
  std::condition_variable cv;
  bool                    resolved = false;
};

/// @brief Stub actor internal state
struct StubActorInternal {
  std::shared_ptr<IActor>       actor;
  std::shared_ptr<StubActorRef> ref;
  std::queue<ActorMessage>      mailbox;
  std::thread                   worker_thread;
  std::mutex                    mailbox_mutex;
  std::condition_variable       mailbox_cv;
  bool                          should_stop = false;
};

/// @brief Stub implementation of IActorSystem
class StubActorSystem : public IActorSystem,
                        public std::enable_shared_from_this<StubActorSystem> {
public:
  StubActorSystem();
  ~StubActorSystem() override;

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

  /// @brief Deliver a message to the actor at the given address
  void deliver_message(const std::string& address, const ActorMessage& msg);

  /// @brief Deliver a message with an explicit sender ref (used by ask)
  void deliver_message_with_sender(const std::string& address, const ActorMessage& msg,
                                   std::shared_ptr<IActorRef> sender_ref);

  /// @brief Register a reply future for an ask request
  void register_reply_future(const std::string& correlation_id,
                             std::shared_ptr<StubReplyFuture> future);

  /// @brief Complete an ask reply future
  void complete_reply_future(const std::string& correlation_id, const ActorMessage& reply);

private:
  std::string                                               system_id_;
  std::map<std::string, std::shared_ptr<StubActorInternal>> actors_;
  mutable std::mutex                                        actors_mutex_;
  bool                                                      running_       = true;
  int                                                       next_actor_id_ = 0;

  // Reply futures for ask pattern
  std::map<std::string, std::shared_ptr<StubReplyFuture>> reply_futures_;
  std::mutex                                              reply_futures_mutex_;

  void actor_worker_loop(std::shared_ptr<StubActorInternal> actor_internal);
};

/// @brief Stub implementation of IContext
class StubContext : public IContext {
public:
  StubContext();
  ~StubContext() override;

  std::shared_ptr<IActorRef> self() override;
  std::shared_ptr<IActorRef> parent() override;
  std::shared_ptr<IActorRef> sender() override;
  ActorMessage               current_message() const override;
  void                       send(IActorRef* target, const ActorMessage& msg) override;
  ActorMessage ask(IActorRef* target, const ActorMessage& msg, int timeout_ms) override;

  void set_self(std::shared_ptr<IActorRef> ref) { self_ = ref; }
  void set_sender(std::shared_ptr<IActorRef> ref) { sender_ = ref; }
  void set_current_message(const ActorMessage& msg) { current_msg_ = msg; }

  /// @brief Bind to the actor system for message routing
  void set_system(std::weak_ptr<StubActorSystem> system) { system_ = system; }

private:
  std::shared_ptr<IActorRef>     self_;
  std::shared_ptr<IActorRef>     parent_;
  std::shared_ptr<IActorRef>     sender_;
  ActorMessage                   current_msg_;
  std::weak_ptr<StubActorSystem> system_;
};

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_ADAPTER_STUB_IMPL_H
