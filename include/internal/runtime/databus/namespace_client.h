// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// NamespaceClientImpl - Namespace-scoped DataBus client implementation

#ifndef AGENTFLOW_RUNTIME_NAMESPACE_CLIENT_H
#define AGENTFLOW_RUNTIME_NAMESPACE_CLIENT_H

#include <string>

#include "agentflow/so_node.h"

namespace agentflow {
namespace runtime {

/**
 * @brief NamespaceClientImpl implements INamespaceClient for namespace-scoped DataBus access.
 *
 * This class automatically prefixes all keys with /session/{session-id}/{namespace}/
 * to provide session isolation.
 *
 * Usage:
 * \code
 * INodeDataBus* databus = ...;
 * NamespaceClientImpl client(databus, "/session/abc123/user/");
 * client.put("my_key", SoDataVariant(std::string("value")));
 * // Actual key: /session/abc123/user/my_key
 * \endcode
 */
class NamespaceClientImpl : public INamespaceClient {
public:
  /// Construct a namespace client with the given DataBus and prefix.
  /// The prefix should be in the form: /session/{session-id}/{namespace}/
  /// The databus pointer must remain valid for the lifetime of this client.
  NamespaceClientImpl(INodeDataBus* databus, const std::string& prefix);

  ~NamespaceClientImpl() override = default;

  // ---- INamespaceClient Interface Implementation ----

  bool put(const std::string& key, const SoDataVariant& value) override;

  bool get(const std::string& key, SoDataVariant& value) const override;

  bool contains(const std::string& key) const override;

  bool remove(const std::string& key) override;

  SubscriptionHandle subscribe(const std::string& key, SoDataBusCallback callback) override;

  void unsubscribe(const SubscriptionHandle& handle) override;

private:
  /// Build full key with prefix.
  /// Returns prefix_ + key
  inline std::string build_key(const std::string& key) const {
    return prefix_ + key;
  }

  INodeDataBus* databus_;
  std::string prefix_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_NAMESPACE_CLIENT_H