// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Ask pattern implementation for request-response messaging

#ifndef AGENTFLOW_ACTOR_ASK_PATTERN_H
#define AGENTFLOW_ACTOR_ASK_PATTERN_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <functional>
#include "agentflow/actor/adapter.h"
#include "agentflow/common.h"

// Forward declare Protoactor types
namespace protoactor {
class PID;
}

namespace agentflow {
namespace actor {

/// @brief Future for asynchronous response
template<typename T>
class ActorFuture {
 public:
  ActorFuture() : completed_(false), timed_out_(false) {}

  /// @brief Wait for the result (blocking)
  /// @param timeout_ms Timeout in milliseconds (0 = infinite)
  /// @return true if completed, false if timed out
  bool wait(int timeout_ms = 0) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (completed_) {
      return true;
    }

    if (timeout_ms <= 0) {
      cv_.wait(lock, [this] { return completed_; });
      return true;
    } else {
      return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return completed_; });
    }
  }

  /// @brief Get the result (blocking until available or timeout)
  /// @param timeout_ms Timeout in milliseconds (0 = infinite)
  /// @param fallback_value Value to return if timed out
  /// @return The result or fallback_value if timed out
  T get(int timeout_ms = 0, T fallback_value = T()) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (completed_) {
      return value_;
    }

    if (timeout_ms <= 0) {
      cv_.wait(lock, [this] { return completed_; });
      return value_;
    } else {
      if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [this] { return completed_; })) {
        return value_;
      } else {
        timed_out_ = true;
        return fallback_value;
      }
    }
  }

  /// @brief Check if the future is completed
  bool is_completed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_;
  }

  /// @brief Check if the future timed out
  bool is_timed_out() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timed_out_;
  }

  /// @brief Set the result (called by Promise)
  void set(const T& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = value;
    completed_ = true;
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  T value_;
  bool completed_;
  bool timed_out_;
};

/// @brief Specialization for ActorMessage (void-like)
template<>
class ActorFuture<ActorMessage> {
 public:
  ActorFuture() : completed_(false), timed_out_(false) {}

  bool wait(int timeout_ms = 0) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (completed_) {
      return true;
    }

    if (timeout_ms <= 0) {
      cv_.wait(lock, [this] { return completed_; });
      return true;
    } else {
      return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return completed_; });
    }
  }

  ActorMessage get(int timeout_ms = 0, ActorMessage fallback_value = ActorMessage()) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (completed_) {
      return value_;
    }

    if (timeout_ms <= 0) {
      cv_.wait(lock, [this] { return completed_; });
      return value_;
    } else {
      if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [this] { return completed_; })) {
        return value_;
      } else {
        timed_out_ = true;
        return fallback_value;
      }
    }
  }

  bool is_completed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_;
  }

  bool is_timed_out() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timed_out_;
  }

  void set(const ActorMessage& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = value;
    completed_ = true;
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  ActorMessage value_;
  bool completed_;
  bool timed_out_;
};

/// @brief Promise for setting future results
template<typename T>
class ActorPromise {
 public:
  explicit ActorPromise(std::shared_ptr<ActorFuture<T>> future)
      : future_(future) {}

  /// @brief Set the result value
  void set_value(const T& value) {
    if (future_) {
      future_->set(value);
    }
  }

  /// @brief Get the associated future
  std::shared_ptr<ActorFuture<T>> get_future() const {
    return future_;
  }

 private:
  std::shared_ptr<ActorFuture<T>> future_;
};

/// @brief Ask request context
struct AskRequest {
  uint32_t request_id;
  std::string correlation_id;
  std::shared_ptr<protoactor::PID> sender_pid;
  std::string message_type;

  AskRequest(uint32_t id, const std::string& corr_id,
             std::shared_ptr<protoactor::PID> pid,
             const std::string& msg_type)
      : request_id(id), correlation_id(corr_id), sender_pid(pid),
        message_type(msg_type) {}
};

/// @brief Ask response context
struct AskResponse {
  uint32_t request_id;
  std::string correlation_id;
  ActorMessage response;

  AskResponse(uint32_t id, const std::string& corr_id,
             const ActorMessage& resp)
      : request_id(id), correlation_id(corr_id), response(resp) {}
};

/// @brief Ask pattern manager - manages pending requests
class AskPatternManager {
 public:
  /// @brief Create a new ask request
  /// @param sender The sender actor ref
  /// @param message_type Message type for the request
  /// @return Pair of (correlation_id, promise)
  template<typename T>
  std::pair<std::string, ActorPromise<T>> create_ask(
      std::shared_ptr<IActorRef> sender,
      const std::string& message_type) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto correlation_id = generate_correlation_id();
    auto future = std::make_shared<ActorFuture<T>>();
    auto promise = ActorPromise<T>(future);

    pending_requests_[correlation_id] = {
      future,
      sender ? sender->address() : "",
      message_type,
      std::chrono::steady_clock::now()
    };

    return {correlation_id, promise};
  }

  /// @brief Complete an ask request with a response
  /// @param correlation_id The correlation ID from the request
  /// @param response The response message
  /// @return true if request was found and completed, false otherwise
  bool complete_request(const std::string& correlation_id,
                     const ActorMessage& response) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_requests_.find(correlation_id);
    if (it == pending_requests_.end()) {
      return false;
    }

    // Complete the future
    auto& future_any = it->second.future;
    auto message_future = std::dynamic_pointer_cast<ActorFuture<ActorMessage>>(future_any);
    if (message_future) {
      message_future->set(response);
    }

    pending_requests_.erase(it);
    return true;
  }

  /// @brief Check if a request is pending
  /// @param correlation_id The correlation ID
  /// @return true if pending
  bool is_pending(const std::string& correlation_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_requests_.find(correlation_id) != pending_requests_.end();
  }

  /// @brief Get count of pending requests
  size_t pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_requests_.size();
  }

  /// @brief Clean up expired requests
  /// @param timeout_ms Timeout in milliseconds
  /// @return Number of requests cleaned up
  size_t cleanup_expired(int timeout_ms = 30000) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);
    size_t cleaned = 0;

    for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
      if (now - it->second.created_at > timeout) {
        auto& future_any = it->second.future;
        auto message_future = std::dynamic_pointer_cast<ActorFuture<ActorMessage>>(future_any);
        if (message_future) {
          // Mark as timed out
          ActorMessage timeout_msg;
          timeout_msg.message_type = "timeout";
          message_future->set(timeout_msg);
        }
        it = pending_requests_.erase(it);
        ++cleaned;
      } else {
        ++it;
      }
    }

    return cleaned;
  }

 private:
  struct PendingRequest {
    std::shared_ptr<void> future;  // ActorFuture<T> as shared_ptr<void>
    std::string sender_address;
    std::string message_type;
    std::chrono::steady_clock::time_point created_at;
  };

  static std::string generate_correlation_id() {
    static std::atomic<uint64_t> counter{1};
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return "ask_" + std::to_string(timestamp) + "_" +
           std::to_string(counter.fetch_add(1));
  }

  mutable std::mutex mutex_;
  std::map<std::string, PendingRequest> pending_requests_;
};

/// @brief Global ask pattern manager instance
AskPatternManager& ask_pattern_manager();

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_ASK_PATTERN_H
