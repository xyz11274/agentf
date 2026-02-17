// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Network adapter factory implementations

#include "agentflow/network/adapter_factory.h"

#include "agentflow/network/ssl_adapter/openssl_impl.h"
#include "agentflow/network/websocket_adapter/libwebsockets_impl.h"

namespace agentflow {
namespace network {

std::shared_ptr<ISslAdapter> create_ssl_adapter() {
#ifdef AGENTFLOW_WITH_OPENSSL
  return std::make_shared<SslOpensslAdapter>();
#else
  return std::make_shared<SslStubAdapter>();
#endif
}

std::shared_ptr<IWebSocketAdapter> create_websocket_adapter() {
#ifdef AGENTFLOW_WITH_LIBWEBSOCKETS
  return std::make_shared<WebSocketLwsAdapter>();
#else
  return std::make_shared<WebSocketStubAdapter>();
#endif
}

}  // namespace network
}  // namespace agentflow
