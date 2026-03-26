// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// NodeDataBusClientImpl - Session-isolated DataBus client implementation

#ifndef AGENTFLOW_RUNTIME_NODE_DATABUS_CLIENT_H
#define AGENTFLOW_RUNTIME_NODE_DATABUS_CLIENT_H

#include <memory>
#include <string>

#include "agentflow/so_node.h"
#include "runtime/databus/namespace_client.h"
#include "runtime/databus/node_databus_adapter.h"

namespace agentflow {
namespace runtime {

/**
 * @brief NodeDataBusClientImpl implements INodeDataBusClient for session-isolated DataBus access.
 *
 * This class provides namespace-scoped access to DataBus with automatic session prefix.
 * All data is stored under /session/{session-id}/{namespace}/ keys.
 *
 * Key structure: /session/{session-id}/{namespace}/{key}
 *
 * Usage:
 * \code
 * MMDataBus* databus = ...;
 * NodeDataBusClientImpl client(databus, "550e8400-e29b-41d4-a716-446655440000");
 *
 * // Store user data
 * client.user()->put("my_key", SoDataVariant(std::string("value")));
 * // Actual key: /session/550e8400-.../user/my_key
 *
 * // Read framework config
 * SoDataVariant api_key;
 * client.sys()->get("api_key", api_key);
 * \endcode
 */
class NodeDataBusClientImpl : public INodeDataBusClient {
public:
  /// Construct a session-isolated client with the given MMDataBus and session ID.
  /// The session_id should be a UUID format string.
  /// The databus pointer must remain valid for the lifetime of this client.
  /// This constructor creates an internal NodeDataBusAdapter.
  NodeDataBusClientImpl(MMDataBus* databus, const std::string& session_id);

  /// Construct with an existing INodeDataBus adapter (for advanced use cases).
  NodeDataBusClientImpl(INodeDataBus* databus, const std::string& session_id);

  ~NodeDataBusClientImpl() override = default;

  // ---- INodeDataBusClient Interface Implementation ----

  INamespaceClient* sys() override;
  INamespaceClient* ctx() override;
  INamespaceClient* llm() override;
  INamespaceClient* tool() override;
  INamespaceClient* user() override;

  const std::string& session_id() const override;

  INodeDataBus* raw() override;

  IAsyncCallSDK* async() override;

  /// Set the AsyncCall SDK instance (called by runtime during node injection)
  void set_async_sdk(std::shared_ptr<IAsyncCallSDK> async_sdk);

private:
  /// Build namespace prefix for the given namespace.
  /// Returns /session/{session-id}/{namespace}/
  std::string build_namespace_prefix(DataBusNamespace ns) const;

  /// Get or create namespace client (lazy initialization).
  INamespaceClient* get_or_create_client(std::unique_ptr<NamespaceClientImpl>& client,
                                         DataBusNamespace ns);

  // Either we own the adapter (owned_adapter_) or use external one (databus_)
  std::shared_ptr<NodeDataBusAdapter> owned_adapter_;
  INodeDataBus* databus_;
  std::string session_id_;

  // Lazy-initialized namespace clients
  std::unique_ptr<NamespaceClientImpl> sys_client_;
  std::unique_ptr<NamespaceClientImpl> ctx_client_;
  std::unique_ptr<NamespaceClientImpl> llm_client_;
  std::unique_ptr<NamespaceClientImpl> tool_client_;
  std::unique_ptr<NamespaceClientImpl> user_client_;

  // AsyncCall SDK (injected by runtime)
  std::shared_ptr<IAsyncCallSDK> async_sdk_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_NODE_DATABUS_CLIENT_H