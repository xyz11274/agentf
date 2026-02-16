// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Network adapter factories

#pragma once

#include "agentflow/network/ssl_adapter.h"
#include "agentflow/network/websocket_adapter.h"
#include <memory>
#include <string>

namespace agentflow {
namespace network {

/**
 * Create SSL adapter instance
 * Uses OpenSSL if available, otherwise returns stub
 */
std::shared_ptr<ISslAdapter> create_ssl_adapter();

/**
 * Create WebSocket adapter instance
 * Uses libwebsockets if available, otherwise returns stub
 */
std::shared_ptr<IWebSocketAdapter> create_websocket_adapter();

}  // namespace network
}  // namespace agentflow
