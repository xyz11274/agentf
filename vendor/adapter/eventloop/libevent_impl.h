// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// libevent-based event loop implementation

#pragma once

#ifdef AGENTFLOW_WITH_LIBEVENT

#include <event2/event.h>
#include <functional>
#include <memory>

#include "event/eventloop_adapter.h"

namespace agentflow {
namespace event {

/**
 * Forward declaration for libevent event
 */
using LibeventEvent = struct event;

/**
 * Timer event using libevent
 */
class EventTimerLibevent : public ITimerEvent {
public:
  explicit EventTimerLibevent(LibeventEvent* ev);
  ~EventTimerLibevent() override;

  bool cancel() override;
  bool is_active() const override;

private:
  LibeventEvent* ev_;
};

/**
 * File descriptor event using libevent
 */
class EventFileLibevent : public IFileEvent {
public:
  EventFileLibevent(int fd, event_base* base);
  ~EventFileLibevent() override;

  bool set_callback(EventCallback cb) override;
  bool enable(int flags) override;
  bool disable(int flags) override;
  bool is_active() const override;

private:
  int            fd_;
  event_base*    base_;
  LibeventEvent* ev_;
  EventCallback  callback_;
};

/**
 * Event loop using libevent
 */
class EventLoopLibevent : public IEventLoop {
public:
  EventLoopLibevent();
  ~EventLoopLibevent() override;

  bool initialize() override;
  int  dispatch(int timeout_ms = -1) override;
  int  dispatch_once(int timeout_ms = 0) override;
  void exit() override;

  std::shared_ptr<IFileEvent> add_fd_event(int fd) override;
  bool                        remove_fd_event(int fd) override;

  std::shared_ptr<ITimerEvent> add_timer(uint32_t              timeout_ms,
                                         std::function<void()> callback) override;

  std::shared_ptr<ITimerEvent> add_persistent_timer(uint32_t              interval_ms,
                                                    std::function<void()> callback) override;

  bool add_callback(std::function<void()> cb) override;

  EventLoopError last_error() const override { return error_; }
  std::string    error_message() const override { return error_msg_; }
  bool           is_running() const override;

private:
  event_base*    base_;
  EventLoopError error_;
  std::string    error_msg_;
};

/**
 * Event loop adapter for libevent
 */
class EventLoopLibeventAdapter : public IEventLoopAdapter {
public:
  EventLoopLibeventAdapter();
  ~EventLoopLibeventAdapter() override;

  std::shared_ptr<IEventLoop> create_loop() override;
  std::string                 get_version() override;
  bool                        is_available() const override { return available_; }
  bool                        initialize() override;
  void                        cleanup() override;

private:
  bool available_;
};

}  // namespace event
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_LIBEVENT
