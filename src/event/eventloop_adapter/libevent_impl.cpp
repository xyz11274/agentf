// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// libevent-based event loop implementation

#ifdef AGENTFLOW_WITH_LIBEVENT

#include "agentflow/event/eventloop_adapter/libevent_impl.h"

#include <cstring>
#include <event2/event.h>
#include <memory>

#include "agentflow/basic/logger.h"

namespace agentflow {
namespace event {

// ============================================================================
// EventTimerLibevent Implementation
// ============================================================================

EventTimerLibevent::EventTimerLibevent(LibeventEvent* ev) : ev_(ev) {
}

EventTimerLibevent::~EventTimerLibevent() {
  if(ev_) {
    event_del(ev_);
    event_free(ev_);
    ev_ = nullptr;
  }
}

bool EventTimerLibevent::cancel() {
  if(!ev_) {
    return false;
  }

  int result = event_del(ev_);
  return result == 0;
}

bool EventTimerLibevent::is_active() const {
  return ev_ != nullptr;
}

// ============================================================================
// EventFileLibevent Implementation
// ============================================================================

EventFileLibevent::EventFileLibevent(int fd, event_base* base)
    : fd_(fd), base_(base), ev_(nullptr) {
}

EventFileLibevent::~EventFileLibevent() {
  if(ev_) {
    event_del(ev_);
    event_free(ev_);
    ev_ = nullptr;
  }
}

bool EventFileLibevent::set_callback(EventCallback cb) {
  callback_ = cb;

  if(!base_ || !callback_) {
    return false;
  }

  // If event already exists, delete and recreate
  if(ev_) {
    event_del(ev_);
    event_free(ev_);
  }

  // Create new event
  ev_ = event_new(base_, fd_, EV_READ | EV_PERSIST, nullptr, nullptr);
  if(!ev_) {
    return false;
  }

  return true;
}

bool EventFileLibevent::enable(int flags) {
  if(!ev_ || !callback_) {
    return false;
  }

  int result = event_add(ev_, nullptr);
  return result == 0;
}

bool EventFileLibevent::disable(int flags) {
  if(!ev_) {
    return false;
  }

  int result = event_del(ev_);
  return result == 0;
}

bool EventFileLibevent::is_active() const {
  return ev_ != nullptr;
}

// ============================================================================
// EventLoopLibevent Implementation
// ============================================================================

EventLoopLibevent::EventLoopLibevent() : base_(nullptr), error_(EventLoopError::EL_OK) {
}

EventLoopLibevent::~EventLoopLibevent() {
  exit();
}

bool EventLoopLibevent::initialize() {
  if(base_) {
    return true;
  }

  base_ = event_base_new();
  if(!base_) {
    error_     = EventLoopError::EL_INIT_FAILED;
    error_msg_ = "Failed to create event base";
    return false;
  }

  Logger::instance().info("EventLoopLibevent initialized");
  return true;
}

int EventLoopLibevent::dispatch(int timeout_ms) {
  if(!base_) {
    error_ = EventLoopError::EL_INIT_FAILED;
    return -1;
  }

  struct timeval tv;
  if(timeout_ms >= 0) {
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
  }

  // Set timeout if specified
  if(timeout_ms >= 0) {
    event_base_loopexit(base_, &tv);
  }

  int result = event_base_loop(base_, EVLOOP_NONBLOCK);
  return result;
}

int EventLoopLibevent::dispatch_once(int timeout_ms) {
  if(!base_) {
    error_ = EventLoopError::EL_INIT_FAILED;
    return -1;
  }

  struct timeval tv;
  if(timeout_ms >= 0) {
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
  }

  // Set timeout if specified
  if(timeout_ms >= 0) {
    event_base_loopexit(base_, &tv);
  }

  int result = event_base_loop(base_, EVLOOP_ONCE);
  return result;
}

void EventLoopLibevent::exit() {
  if(base_) {
    event_base_loopbreak(base_);
    event_base_free(base_);
    base_ = nullptr;
    Logger::instance().info("EventLoopLibevent exited");
  }
}

std::shared_ptr<IFileEvent> EventLoopLibevent::add_fd_event(int fd) {
  if(!base_) {
    error_ = EventLoopError::EL_INIT_FAILED;
    return nullptr;
  }

  auto file_event = std::make_shared<EventFileLibevent>(fd, base_);
  return file_event;
}

bool EventLoopLibevent::remove_fd_event(int fd) {
  // File events are managed by their own lifetime
  // This is a placeholder implementation
  return true;
}

std::shared_ptr<ITimerEvent> EventLoopLibevent::add_timer(uint32_t              timeout_ms,
                                                          std::function<void()> callback) {
  if(!base_) {
    error_ = EventLoopError::EL_INIT_FAILED;
    return nullptr;
  }

  auto ev = event_new(base_, -1, 0, nullptr, nullptr);
  if(!ev) {
    error_ = EventLoopError::EL_TIMER_FAILED;
    return nullptr;
  }

  struct timeval tv;
  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int result = event_add(ev, &tv);
  if(result != 0) {
    event_free(ev);
    return nullptr;
  }

  return std::make_shared<EventTimerLibevent>(ev);
}

std::shared_ptr<ITimerEvent> EventLoopLibevent::add_persistent_timer(
    uint32_t interval_ms, std::function<void()> callback) {
  if(!base_) {
    error_ = EventLoopError::EL_INIT_FAILED;
    return nullptr;
  }

  auto ev = event_new(base_, -1, EV_PERSIST, nullptr, nullptr);
  if(!ev) {
    error_ = EventLoopError::EL_TIMER_FAILED;
    return nullptr;
  }

  struct timeval tv;
  tv.tv_sec  = interval_ms / 1000;
  tv.tv_usec = (interval_ms % 1000) * 1000;

  int result = event_add(ev, &tv);
  if(result != 0) {
    event_free(ev);
    return nullptr;
  }

  return std::make_shared<EventTimerLibevent>(ev);
}

bool EventLoopLibevent::add_callback(std::function<void()> cb) {
  if(!cb) {
    return false;
  }

  // Add a one-shot timer that immediately fires
  return add_timer(0, cb) != nullptr;
}

bool EventLoopLibevent::is_running() const {
  return base_ != nullptr;
}

// ============================================================================
// EventLoopLibeventAdapter Implementation
// ============================================================================

EventLoopLibeventAdapter::EventLoopLibeventAdapter() : available_(false) {
  // Check if libevent is available by attempting to create an event base
  event_base* test_base = event_base_new();
  if(test_base) {
    available_ = true;
    event_base_free(test_base);
  }
}

EventLoopLibeventAdapter::~EventLoopLibeventAdapter() {
  cleanup();
}

std::shared_ptr<IEventLoop> EventLoopLibeventAdapter::create_loop() {
  if(!available_) {
    return nullptr;
  }

  return std::make_shared<EventLoopLibevent>();
}

std::string EventLoopLibeventAdapter::get_version() {
#ifdef LIBEVENT_VERSION
  return LIBEVENT_VERSION;
#else
  return "unknown";
#endif
}

bool EventLoopLibeventAdapter::initialize() {
  return available_;
}

void EventLoopLibeventAdapter::cleanup() {
  // Adapter-level cleanup
}

}  // namespace event
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_LIBEVENT
