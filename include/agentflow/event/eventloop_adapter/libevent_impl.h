// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// libevent-based event loop implementation

#pragma once

#include "agentflow/event/eventloop_adapter.h"

#ifdef AGENTFLOW_WITH_LIBEVENT
#include <event2/event.h>
#endif

namespace agentflow {
namespace event {

#ifdef AGENTFLOW_WITH_LIBEVENT

/**
 * libevent-based timer event implementation
 */
class EventTimerLibevent : public ITimerEvent {
 public:
  explicit EventTimerLibevent(event* ev);
  ~EventTimerLibevent() override;

  bool cancel() override;
  bool is_active() const override;

 private:
  event* ev_;
};

/**
 * libevent-based file descriptor event implementation
 */
class EventFileLibevent : public IFileEvent {
 public:
  explicit EventFileLibevent(int fd, event_base* base);
  ~EventFileLibevent() override;

  bool set_callback(EventCallback cb) override;
  bool enable(int flags) override;
  bool disable(int flags) override;
  bool is_active() const override;

 private:
  int fd_;
  event_base* base_;
  event* ev_;
  EventCallback callback_;
};

/**
 * libevent-based event loop implementation
 */
class EventLoopLibevent : public IEventLoop {
 public:
  EventLoopLibevent();
  ~EventLoopLibevent() override;

  bool initialize() override;
  int dispatch(int timeout_ms = -1) override;
  int dispatch_once(int timeout_ms = 0) override;
  void exit() override;
  std::shared_ptr<IFileEvent> add_fd_event(int fd) override;
  bool remove_fd_event(int fd) override;
  std::shared_ptr<ITimerEvent> add_timer(uint32_t timeout_ms,
                                         std::function<void()> callback) override;
  std::shared_ptr<ITimerEvent> add_persistent_timer(
      uint32_t interval_ms, std::function<void()> callback) override;
  bool add_callback(std::function<void()> cb) override;
  EventLoopError last_error() const override { return error_; }
  std::string error_message() const override { return error_msg_; }
  bool is_running() const override;

 private:
  event_base* base_;
  mutable EventLoopError error_;
  mutable std::string error_msg_;
};

/**
 * libevent adapter implementation
 */
class EventLoopLibeventAdapter : public IEventLoopAdapter {
 public:
  EventLoopLibeventAdapter();
  ~EventLoopLibeventAdapter() override;

  std::shared_ptr<IEventLoop> create_loop() override;
  std::string get_version() override;
  bool is_available() const override { return available_; }
  bool initialize() override;
  void cleanup() override;

 private:
  bool available_;
};

#else

/**
 * Stub timer event for when libevent is not available
 */
class EventTimerStub : public ITimerEvent {
 public:
  bool cancel() override { return false; }
  bool is_active() const override { return false; }
};

/**
 * Stub file event for when libevent is not available
 */
class EventFileStub : public IFileEvent {
 public:
  bool set_callback(EventCallback) override { return false; }
  bool enable(int) override { return false; }
  bool disable(int) override { return false; }
  bool is_active() const override { return false; }
};

/**
 * Stub event loop for when libevent is not available
 */
class EventLoopStub : public IEventLoop {
 public:
  bool initialize() override { return false; }
  int dispatch(int = -1) override { return -1; }
  int dispatch_once(int = 0) override { return -1; }
  void exit() override {}
  std::shared_ptr<IFileEvent> add_fd_event(int) override { return nullptr; }
  bool remove_fd_event(int) override { return false; }
  std::shared_ptr<ITimerEvent> add_timer(uint32_t, std::function<void()>) override {
    return nullptr;
  }
  std::shared_ptr<ITimerEvent> add_persistent_timer(uint32_t,
                                                     std::function<void()>) override {
    return nullptr;
  }
  bool add_callback(std::function<void()>) override { return false; }
  EventLoopError last_error() const override { return EventLoopError::EL_INIT_FAILED; }
  std::string error_message() const override { return "libevent not available"; }
  bool is_running() const override { return false; }
};

/**
 * Stub event loop adapter
 */
class EventLoopStubAdapter : public IEventLoopAdapter {
 public:
  std::shared_ptr<IEventLoop> create_loop() override {
    return std::make_shared<EventLoopStub>();
  }
  std::string get_version() override { return "stub"; }
  bool is_available() const override { return false; }
  bool initialize() override { return false; }
  void cleanup() override {}
};

#endif  // AGENTFLOW_WITH_LIBEVENT

}  // namespace event
}  // namespace agentflow
