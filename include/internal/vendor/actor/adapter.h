// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Actor adapter layer - defines interfaces independent of Protoactor
// All AgentFlow components depend on this adapter, not on Protoactor directly

#ifndef AGENTFLOW_ACTOR_ADAPTER_H
#define AGENTFLOW_ACTOR_ADAPTER_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace actor {
namespace adapter {

// Forward declarations
class IActorRef;
class IContext;
class IActor;
class IActorSystem;
class Message;

/// @brief Generic message envelope independent of actor framework
struct ActorMessage {
  std::string           message_type;
  std::shared_ptr<void> payload;

  // Optional routing fields used by stub and protoactor adapters
  std::string sender_address;    ///< Address of the sender (for ask/reply routing)
  std::string reply_to_address;  ///< Correlation ID for ask pattern reply routing

  template <typename T>
  std::shared_ptr<T> get_payload() const {
    return std::static_pointer_cast<T>(payload);
  }
};

/// @brief Actor reference - safe way to address actors
class IActorRef {
public:
  virtual ~IActorRef() = default;

  /// @brief Get the address of this actor
  /// @return String address/path
  virtual std::string address() const = 0;

  /// @brief Check if reference is still valid
  /// @return true if actor is still alive
  virtual bool is_alive() const = 0;

  /// @brief Get a string representation
  virtual std::string to_string() const = 0;
};

/// @brief Actor execution context - framework-independent
class IContext {
public:
  virtual ~IContext() = default;

  /// @brief Get reference to self
  /// @return Self actor reference
  virtual std::shared_ptr<IActorRef> self() = 0;

  /// @brief Get reference to parent
  /// @return Parent actor reference or nullptr
  virtual std::shared_ptr<IActorRef> parent() = 0;

  /// @brief Get reference to sender of current message
  /// @return Sender reference or nullptr
  virtual std::shared_ptr<IActorRef> sender() = 0;

  /// @brief Get current message
  /// @return Current message
  virtual ActorMessage current_message() const = 0;

  /// @brief Send message to another actor
  /// @param target The target actor
  /// @param msg The message to send
  virtual void send(IActorRef* target, const ActorMessage& msg) = 0;

  /// @brief Send message and ask for reply
  /// @param target The target actor
  /// @param msg The message
  /// @param timeout_ms Timeout in milliseconds
  /// @return Future result or empty if timeout
  virtual ActorMessage ask(IActorRef* target, const ActorMessage& msg, int timeout_ms) = 0;
};

/// @brief Abstract actor interface - framework-independent
class IActor {
public:
  virtual ~IActor() = default;

  /// @brief Called when actor receives a message
  /// @param ctx The context for this execution
  virtual void on_message(std::shared_ptr<IContext> ctx) = 0;

  /// @brief Called when actor is started
  virtual void on_start() {}

  /// @brief Called when actor is about to stop
  virtual void on_stop() {}

  /// @brief Get actor type/name for debugging
  /// @return Actor type
  virtual std::string actor_type() const { return typeid(*this).name(); }
};

/// @brief Actor factory function type
using ActorFactory = std::function<std::shared_ptr<IActor>()>;

/// @brief Actor system interface - framework-independent
class IActorSystem {
public:
  virtual ~IActorSystem() = default;

  /// @brief Spawn a new actor
  /// @param factory Factory function to create actor
  /// @param actor_name Optional actor name for addressing
  /// @return Reference to spawned actor
  virtual std::shared_ptr<IActorRef> spawn(const ActorFactory& factory,
                                           const std::string&  actor_name = "") = 0;

  /// @brief Spawn an actor with configuration
  /// @param factory Factory function
  /// @param actor_name Actor name
  /// @param mailbox_size Max mailbox size (0 = unlimited)
  /// @return Reference to spawned actor
  virtual std::shared_ptr<IActorRef> spawn_with_config(const ActorFactory& factory,
                                                       const std::string&  actor_name,
                                                       int                 mailbox_size) = 0;

  /// @brief Get an actor by address
  /// @param address Actor address
  /// @return Reference to actor or nullptr
  virtual std::shared_ptr<IActorRef> get_actor(const std::string& address) = 0;

  /// @brief Stop an actor
  /// @param actor_ref Reference to actor
  virtual void stop(std::shared_ptr<IActorRef> actor_ref) = 0;

  /// @brief Shutdown the entire actor system
  virtual void shutdown() = 0;

  /// @brief Check if system is running
  /// @return true if system is running
  virtual bool is_running() const = 0;

  /// @brief Get system address
  /// @return System address/ID
  virtual std::string system_address() const = 0;

  /// @brief Wait for system to shutdown (blocking)
  /// @param timeout_ms Max time to wait in milliseconds (0 = infinite)
  /// @return true if shutdown completed, false if timeout
  virtual bool wait_shutdown(int timeout_ms = 0) = 0;
};

/// @brief Factory for creating actor system instances
/// This layer provides version/implementation isolation
class ActorSystemFactory {
public:
  /// @brief Create a new actor system
  /// @param system_name Optional system name
  /// @return Shared pointer to actor system
  static std::shared_ptr<IActorSystem> create(const std::string& system_name = "agentflow-system");

  /// @brief Create with custom configuration
  /// @param system_name System name
  /// @param num_workers Number of worker threads (0 = auto-detect)
  /// @return Shared pointer to actor system
  static std::shared_ptr<IActorSystem> create_with_workers(const std::string& system_name,
                                                           int                num_workers);

  /// @brief Get implementation name
  /// @return Name of actor framework (e.g., "protoactor", "stub")
  static std::string implementation_name();

  /// @brief Get implementation version
  /// @return Version string
  static std::string implementation_version();
};

/// @brief Utility for creating messages
class MessageBuilder {
public:
  /// @brief Create message of specific type
  template <typename T>
  static ActorMessage create(const std::string& type, const T& payload) {
    ActorMessage msg;
    msg.message_type = type;
    msg.payload      = std::make_shared<T>(payload);
    return msg;
  }

  /// @brief Create message with shared_ptr payload
  static ActorMessage create(const std::string& type, std::shared_ptr<void> payload) {
    ActorMessage msg;
    msg.message_type = type;
    msg.payload      = payload;
    return msg;
  }
};

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_ACTOR_ADAPTER_H
