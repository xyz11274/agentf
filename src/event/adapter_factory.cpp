// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Event loop adapter factory implementation

#include "agentflow/event/adapter_factory.h"

#include "agentflow/event/eventloop_adapter/libevent_impl.h"

namespace agentflow {
namespace event {

std::shared_ptr<IEventLoopAdapter> create_eventloop_adapter() {
#ifdef AGENTFLOW_WITH_LIBEVENT
  return std::make_shared<EventLoopLibeventAdapter>();
#else
  // Return nullptr for now - stub implementation needed
  return nullptr;
#endif
}

}  // namespace event
}  // namespace agentflow
