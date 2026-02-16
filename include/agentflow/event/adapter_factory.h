// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Event loop adapter factory

#pragma once

#include "agentflow/event/eventloop_adapter.h"
#include <memory>

namespace agentflow {
namespace event {

/**
 * Create event loop adapter instance
 * Uses libevent if available, otherwise returns stub
 */
std::shared_ptr<IEventLoopAdapter> create_eventloop_adapter();

}  // namespace event
}  // namespace agentflow
