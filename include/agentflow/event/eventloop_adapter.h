// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Event loop adapter interface for libevent or other event-driven implementations

#pragma once

#include <memory>
#include <string>
#include <functional>
#include <cstdint>

namespace agentflow {
namespace event {

enum class EventLoopError {
  EL_OK = 0,
  EL_INIT_FAILED = 1,
  EL_ADD_EVENT_FAILED = 2,
  EL_REMOVE_EVENT_FAILED = 3,
  EL_DISPATCH_FAILED = 4,
  EL_TIMER_FAILED = 5,
};

// Event flags
enum class EventFlag {
  EV_READ = 0x01,
  EV_WRITE = 0x02,
  EV_PERSIST = 0x10,  // Persistent event
};

/**
 * Timer event handle
 */
class ITimerEvent {
 public:
  virtual ~ITimerEvent() = default;
  virtual bool cancel() = 0;
  virtual bool is_active() const = 0;
};

/**
 * File descriptor event interface
 */
class IFileEvent {
 public:
  virtual ~IFileEvent() = default;
  using EventCallback = std::function<void(int fd, int flags)>;

  virtual bool set_callback(EventCallback cb) = 0;
  virtual bool enable(int flags) = 0;
  virtual bool disable(int flags) = 0;
  virtual bool is_active() const = 0;
};

/**
 * Event loop interface
 */
class IEventLoop {
 public:
  virtual ~IEventLoop() = default;

  /**
   * Initialize event loop
   */
  virtual bool initialize() = 0;

  /**
   * Dispatch events (blocking, runs for given time or until no events)
   */
  virtual int dispatch(int timeout_ms = -1) = 0;

  /**
   * Dispatch single event
   */
  virtual int dispatch_once(int timeout_ms = 0) = 0;

  /**
   * Exit event loop
   */
  virtual void exit() = 0;

  /**
   * Add file descriptor event
   */
  virtual std::shared_ptr<IFileEvent> add_fd_event(int fd) = 0;

  /**
   * Remove file descriptor event
   */
  virtual bool remove_fd_event(int fd) = 0;

  /**
   * Add timer event (one-shot)
   */
  virtual std::shared_ptr<ITimerEvent> add_timer(
      uint32_t timeout_ms,
      std::function<void()> callback) = 0;

  /**
   * Add persistent timer event
   */
  virtual std::shared_ptr<ITimerEvent> add_persistent_timer(
      uint32_t interval_ms,
      std::function<void()> callback) = 0;

  /**
   * Add callback to be executed in event loop
   */
  virtual bool add_callback(std::function<void()> cb) = 0;

  /**
   * Get last error
   */
  virtual EventLoopError last_error() const = 0;

  /**
   * Get error message
   */
  virtual std::string error_message() const = 0;

  /**
   * Check if running
   */
  virtual bool is_running() const = 0;
};

/**
 * Event loop adapter interface
 * Isolates libevent or other event frameworks from AgentFlow core
 */
class IEventLoopAdapter {
 public:
  virtual ~IEventLoopAdapter() = default;

  /**
   * Create new event loop
   */
  virtual std::shared_ptr<IEventLoop> create_loop() = 0;

  /**
   * Get library version
   */
  virtual std::string get_version() = 0;

  /**
   * Check if event loop support is available
   */
  virtual bool is_available() const = 0;

  /**
   * Initialize adapter
   */
  virtual bool initialize() = 0;

  /**
   * Cleanup adapter
   */
  virtual void cleanup() = 0;
};

}  // namespace event
}  // namespace agentflow
