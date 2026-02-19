// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Actor system manager for AgentFlow

#ifndef AGENTFLOW_ACTOR_SYSTEM_H
#define AGENTFLOW_ACTOR_SYSTEM_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace protoactor {
class ActorSystem;
class PID;
}  // namespace protoactor

namespace agentflow {

/// @brief ActorSystemManager manages the Protoactor-cpp runtime
class ActorSystemManager {
public:
  /// @brief Get or create singleton instance
  /// @return ActorSystemManager instance
  static ActorSystemManager& get_instance();

  /// @brief Initialize the actor system
  /// @return true if initialization successful
  bool initialize();

  /// @brief Shutdown the actor system
  void shutdown();

  /// @brief Check if actor system is running
  /// @return true if running
  bool is_running() const;

  /// @brief Get the underlying Protoactor ActorSystem
  /// @return Pointer to ActorSystem or nullptr if not initialized
  std::shared_ptr<protoactor::ActorSystem> get_actor_system() const;

private:
  ActorSystemManager() = default;
  ~ActorSystemManager();

  // Prevent copying
  ActorSystemManager(const ActorSystemManager&)            = delete;
  ActorSystemManager& operator=(const ActorSystemManager&) = delete;

  mutable std::mutex                       mutex_;
  std::shared_ptr<protoactor::ActorSystem> actor_system_;
  bool                                     initialized_ = false;
};

}  // namespace agentflow

#else

// Stub implementation when Protoactor-cpp is not available
#include <memory>

namespace agentflow {

class ActorSystemManager {
public:
  static ActorSystemManager& get_instance();

  bool                  initialize();
  void                  shutdown();
  bool                  is_running() const;
  std::shared_ptr<void> get_actor_system() const;

private:
  ActorSystemManager() = default;
  ~ActorSystemManager();
};

}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_SYSTEM_H
