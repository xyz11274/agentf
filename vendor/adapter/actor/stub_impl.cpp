// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Stub actor system implementation – used when Protoactor-cpp is not available.
// Provides a fully functional in-process actor system with per-actor thread
// mailboxes, synchronous ask/reply via futures, and proper lifecycle callbacks.

#ifndef AGENTFLOW_WITH_PROTOACTOR

#include "vendor/actor/stub_impl.h"

#include <atomic>
#include <chrono>
#include <sstream>

namespace agentflow {
namespace actor {
namespace adapter {

// ============================================================================
// StubActorRef implementation
// ============================================================================

StubActorRef::StubActorRef(const std::string& address) : address_(address) {}

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

// ============================================================================
// StubContext implementation
// ============================================================================

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
  if(!target) {
    return;
  }

  auto system = system_.lock();
  if(!system) {
    return;
  }

  const std::string& addr = target->address();

  // Route replies for ask pattern: addresses starting with "__reply_future:" are handled
  // by the system's future registry rather than a real actor mailbox.
  if(addr.rfind("__reply_future:", 0) == 0) {
    std::string correlation_id = addr.substr(15);
    system->complete_reply_future(correlation_id, msg);
    return;
  }

  system->deliver_message(addr, msg);
}

ActorMessage StubContext::ask(IActorRef* target, const ActorMessage& msg, int timeout_ms) {
  ActorMessage error_result;
  error_result.message_type = "error";

  if(!target) {
    return error_result;
  }

  auto system = system_.lock();
  if(!system) {
    return error_result;
  }

  // Generate a unique correlation ID for this ask
  static std::atomic<uint64_t> ask_counter{ 1 };
  const std::string correlation_id =
      "__ask_" + std::to_string(ask_counter.fetch_add(1, std::memory_order_relaxed));

  // Register a reply future in the system before sending the message
  auto reply_future = std::make_shared<StubReplyFuture>();
  system->register_reply_future(correlation_id, reply_future);

  // Create a synthetic sender ref whose address maps to the reply future.
  // The receiving actor replies via ctx->send(ctx->sender(), reply_msg),
  // which will be routed to complete_reply_future().
  auto reply_to_ref = std::make_shared<StubActorRef>("__reply_future:" + correlation_id);

  // Deliver the message with the reply-to ref as the sender
  system->deliver_message_with_sender(target->address(), msg, reply_to_ref);

  // Block until the reply arrives or timeout expires
  const int effective_timeout = timeout_ms > 0 ? timeout_ms : 30000;
  {
    std::unique_lock<std::mutex> lock(reply_future->mutex);
    const bool got_reply = reply_future->cv.wait_for(
        lock, std::chrono::milliseconds(effective_timeout),
        [&reply_future] { return reply_future->resolved; });

    if(got_reply) {
      return reply_future->reply;
    }
  }

  ActorMessage timeout_result;
  timeout_result.message_type = "timeout";
  return timeout_result;
}

// ============================================================================
// StubActorSystem implementation
// ============================================================================

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
                                                              int mailbox_size) {
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

  for(auto& pair : actors_) {
    pair.second->should_stop = true;
    pair.second->mailbox_cv.notify_all();
  }

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
        return false;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return true;
}

void StubActorSystem::deliver_message(const std::string& address, const ActorMessage& msg) {
  // Intercept reply-future deliveries
  if(address.rfind("__reply_future:", 0) == 0) {
    complete_reply_future(address.substr(15), msg);
    return;
  }

  std::lock_guard<std::mutex> lock(actors_mutex_);
  auto it = actors_.find(address);
  if(it != actors_.end() && !it->second->should_stop) {
    std::lock_guard<std::mutex> mb_lock(it->second->mailbox_mutex);
    it->second->mailbox.push(msg);
    it->second->mailbox_cv.notify_one();
  }
}

void StubActorSystem::deliver_message_with_sender(const std::string&         address,
                                                  const ActorMessage&        msg,
                                                  std::shared_ptr<IActorRef> sender_ref) {
  // Embed the sender address in the message so the worker loop can restore it
  ActorMessage wrapped     = msg;
  wrapped.sender_address   = sender_ref ? sender_ref->address() : "";
  deliver_message(address, wrapped);
}

void StubActorSystem::register_reply_future(const std::string&               correlation_id,
                                            std::shared_ptr<StubReplyFuture> future) {
  std::lock_guard<std::mutex> lock(reply_futures_mutex_);
  reply_futures_[correlation_id] = future;
}

void StubActorSystem::complete_reply_future(const std::string&  correlation_id,
                                            const ActorMessage& reply) {
  std::shared_ptr<StubReplyFuture> future;
  {
    std::lock_guard<std::mutex> lock(reply_futures_mutex_);
    auto it = reply_futures_.find(correlation_id);
    if(it == reply_futures_.end()) {
      return;
    }
    future = it->second;
    reply_futures_.erase(it);
  }

  {
    std::lock_guard<std::mutex> lock(future->mutex);
    future->reply    = reply;
    future->resolved = true;
  }
  future->cv.notify_all();
}

void StubActorSystem::actor_worker_loop(std::shared_ptr<StubActorInternal> actor_internal) {
  auto context = std::make_shared<StubContext>();
  context->set_self(actor_internal->ref);
  context->set_system(weak_from_this());

  if(actor_internal->actor) {
    actor_internal->actor->on_start();
  }

  while(!actor_internal->should_stop) {
    std::unique_lock<std::mutex> lock(actor_internal->mailbox_mutex);

    actor_internal->mailbox_cv.wait_for(lock, std::chrono::milliseconds(100), [&actor_internal] {
      return !actor_internal->mailbox.empty() || actor_internal->should_stop;
    });

    if(!actor_internal->mailbox.empty()) {
      ActorMessage msg = actor_internal->mailbox.front();
      actor_internal->mailbox.pop();

      // Restore the sender ref from the embedded sender_address field
      if(!msg.sender_address.empty()) {
        context->set_sender(std::make_shared<StubActorRef>(msg.sender_address));
      } else {
        context->set_sender(nullptr);
      }

      context->set_current_message(msg);
      lock.unlock();

      if(actor_internal->actor) {
        try {
          actor_internal->actor->on_message(context);
        } catch(const std::exception&) {
          // Swallow exceptions; actor isolation prevents cascading failures
        }
      }
    }
  }

  if(actor_internal->actor) {
    actor_internal->actor->on_stop();
  }
}

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
