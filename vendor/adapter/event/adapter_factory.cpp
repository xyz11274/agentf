// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Event loop adapter factory implementation

#include "vendor/event/adapter_factory.h"

#include "vendor/event/libevent_impl.h"

namespace agentflow {
namespace event {

std::shared_ptr<IEventLoopAdapter> create_eventloop_adapter() {
#ifdef AGENTFLOW_WITH_LIBEVENT
  return std::make_shared<EventLoopLibeventAdapter>();
#else
  return std::make_shared<EventLoopStubAdapter>();
#endif
}

}  // namespace event
}  // namespace agentflow
