// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SO Library Node Entry Point Definitions
// This header provides ABI-stable interfaces for SO library nodes.

#ifndef AGENTFLOW_SO_NODE_H
#define AGENTFLOW_SO_NODE_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace agentflow {

// ============================================================================
// DataVariant and DataMap - Re-export from framework for ABI compatibility
// ============================================================================

// Note: These types must match the definitions in agent/common.h
// SO libraries should include this header for ABI stability
// The framework will provide the actual definitions

/// DataVariant is a tagged union for passing data between framework and SO nodes.
/// This type must remain ABI-compatible across framework versions.
/// Definition matches agent/common.h
using SoDataVariant = std::variant<std::monostate, bool, int64_t, double, std::string>;

/// DataMap is a dictionary of named data variants.
using SoDataMap = std::map<std::string, SoDataVariant>;

// ============================================================================
// INodeDataBus - Virtual interface for SO library to access DataBus
// ============================================================================

/// Subscription handle returned by subscribe operations.
struct SubscriptionHandle {
  uint64_t id = 0;

  bool is_valid() const { return id != 0; }
  void reset() { id = 0; }

  bool operator==(const SubscriptionHandle& other) const { return id == other.id; }
  bool operator!=(const SubscriptionHandle& other) const { return id != other.id; }
};

/// Callback type for data change notifications.
using SoDataBusCallback = std::function<void(const std::string& key, const SoDataVariant& value)>;

/// INodeDataBus provides a virtual interface for SO library nodes to access
/// the framework's DataBus. This allows nodes to share data with other nodes
/// and communicate with the runtime.
///
/// Usage in SO library node:
/// \code
/// extern "C" void my_node_v2(const SoDataMap& inputs, SoDataMap& outputs, INodeDataBus* databus) {
///     // Read data from databus
///     SoDataVariant value;
///     if (databus && databus->get("shared_key", value)) {
///         // Use the value
///     }
///
///     // Write data to databus for other nodes
///     if (databus) {
///         databus->put("my_output", SoDataVariant(std::string("result")));
///     }
///
///     // Publish event
///     if (databus) {
///         databus->publish("topic.my_event", SoDataVariant(std::string("event_data")));
///     }
/// }
/// \endcode
class INodeDataBus {
public:
  virtual ~INodeDataBus() = default;

  // ---- Key-Value Operations ----

  /// Store a value with the given key.
  /// Returns true if successful.
  virtual bool put(const std::string& key, const SoDataVariant& value) = 0;

  /// Retrieve a value by key.
  /// Returns true if the key exists and value is set.
  virtual bool get(const std::string& key, SoDataVariant& value) const = 0;

  /// Check if a key exists.
  virtual bool contains(const std::string& key) const = 0;

  /// Remove a key-value pair.
  /// Returns true if the key was removed.
  virtual bool remove(const std::string& key) = 0;

  // ---- Event/Topic Operations ----

  /// Publish a value to a topic.
  /// Subscribers to this topic will receive the value.
  virtual bool publish(const std::string& topic, const SoDataVariant& value) = 0;

  // ---- Subscription Operations ----

  /// Subscribe to data changes for a specific key.
  /// Returns a handle that can be used to unsubscribe.
  virtual SubscriptionHandle subscribe(const std::string& key, SoDataBusCallback callback) = 0;

  /// Subscribe to all events on a topic.
  /// Returns a handle that can be used to unsubscribe.
  virtual SubscriptionHandle subscribe_topic(const std::string& topic, SoDataBusCallback callback) = 0;

  /// Unsubscribe using a previously returned handle.
  virtual void unsubscribe(const SubscriptionHandle& handle) = 0;

  /// Unsubscribe from all subscriptions.
  virtual void unsubscribe_all() = 0;

  // ---- Directory Access (for script nodes compatibility) ----

  /// Get the KV directory path for file-based storage.
  /// Returns nullptr if not available.
  virtual const char* kv_dir() const = 0;

  /// Get the events directory path for file-based events.
  /// Returns nullptr if not available.
  virtual const char* events_dir() const = 0;
};

// ============================================================================
// SO Node Entry Point Signatures
// ============================================================================

/// V1 signature: Basic node with inputs and outputs.
/// The framework loads the SO and calls this function with input data.
/// Output data should be written to the outputs map.
///
/// \param inputs Input data from the framework
/// \param outputs Output data to return to the framework
using SoNodeFunctionV1 = void (*)(const SoDataMap& inputs, SoDataMap& outputs);

/// V2 signature: Node with inputs, outputs, and DataBus access.
/// The DataBus allows the node to share data with other nodes and
/// communicate with the runtime.
///
/// \param inputs Input data from the framework
/// \param outputs Output data to return to the framework
/// \param databus Pointer to the DataBus interface (may be null)
using SoNodeFunctionV2 = void (*)(const SoDataMap& inputs, SoDataMap& outputs, INodeDataBus* databus);

// ============================================================================
// DataBus Namespace and Session Isolation (V3)
// ============================================================================

/// Namespaces for session-isolated data storage.
/// Each namespace provides a separate key space within a session.
enum class DataBusNamespace {
  SYS,   ///< Framework communication (response, api_key, config)
  CTX,   ///< Context storage (history, state) - reserved
  LLM,   ///< LLM service interaction (last_prompt, last_response) - reserved
  TOOL,  ///< Tool call information (call_result, tool_list) - reserved
  USER   ///< User-defined node data (custom keys)
};

/// Forward declaration for INodeDataBusClient
class INodeDataBusClient;

/// Namespace-scoped client for a specific DataBus namespace.
/// Provides key-value operations with automatic namespace prefix.
///
/// All keys are automatically prefixed with /session/{session-id}/{namespace}/
///
/// Usage:
/// \code
/// // Store data in user namespace
/// client->put("my_key", SoDataVariant(std::string("value")));
/// // Actual key: /session/{id}/user/my_key
/// \endcode
class INamespaceClient {
public:
  virtual ~INamespaceClient() = default;

  /// Store a value with the given key.
  /// Returns true if successful.
  virtual bool put(const std::string& key, const SoDataVariant& value) = 0;

  /// Retrieve a value by key.
  /// Returns true if the key exists and value is set.
  virtual bool get(const std::string& key, SoDataVariant& value) const = 0;

  /// Check if a key exists.
  virtual bool contains(const std::string& key) const = 0;

  /// Remove a key-value pair.
  /// Returns true if the key was removed.
  virtual bool remove(const std::string& key) = 0;

  /// Subscribe to data changes for a specific key.
  /// Returns a handle that can be used to unsubscribe.
  virtual SubscriptionHandle subscribe(const std::string& key, SoDataBusCallback callback) = 0;

  /// Unsubscribe using a previously returned handle.
  virtual void unsubscribe(const SubscriptionHandle& handle) = 0;
};

/// Client interface for session-isolated DataBus access.
/// Provides namespace-scoped access with automatic session prefix.
///
/// Key structure: /session/{session-id}/{namespace}/{key}
///
/// Usage:
/// \code
/// extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs, INodeDataBusClient* client) {
///     // Store user data
///     client->user()->put("result", SoDataVariant(std::string("success")));
///
///     // Read framework config
///     SoDataVariant api_key;
///     if (client->sys()->get("api_key", api_key)) {
///         // Use API key...
///     }
///
///     // Access raw DataBus (bypasses session isolation)
///     INodeDataBus* raw = client->raw();
///     raw->put("global_key", SoDataVariant(std::string("value")));
/// }
/// \endcode
class INodeDataBusClient {
public:
  virtual ~INodeDataBusClient() = default;

  /// Access sys namespace (framework communication).
  /// Framework messages and configuration like response, api_key.
  virtual INamespaceClient* sys() = 0;

  /// Access ctx namespace (context storage).
  /// Reserved for future use: history, state.
  virtual INamespaceClient* ctx() = 0;

  /// Access llm namespace (LLM service).
  /// Reserved for LLM interaction: last_prompt, last_response.
  virtual INamespaceClient* llm() = 0;

  /// Access tool namespace (tool calls).
  /// Reserved for tool information: call_result, tool_list.
  virtual INamespaceClient* tool() = 0;

  /// Access user namespace (custom data).
  /// User-defined node data with custom keys.
  virtual INamespaceClient* user() = 0;

  /// Get current session ID.
  virtual const std::string& session_id() const = 0;

  /// Get raw DataBus access (bypasses session isolation).
  /// Use with caution - keys are not prefixed.
  /// Primarily for reading global configuration.
  virtual INodeDataBus* raw() = 0;
};

/// V3 signature: Node with session-isolated DataBus client.
/// Recommended for new nodes requiring session isolation.
///
/// \param inputs Input data from the framework
/// \param outputs Output data to return to the framework
/// \param client Session-isolated DataBus client (may be null)
using SoNodeFunctionV3 = void (*)(const SoDataMap& inputs, SoDataMap& outputs, INodeDataBusClient* client);

// ============================================================================
// Helper Functions for SoDataVariant
// ============================================================================

namespace so_node {

/// Convert SoDataVariant to string representation.
inline std::string to_string(const SoDataVariant& var) {
  if (std::holds_alternative<std::string>(var)) {
    return std::get<std::string>(var);
  } else if (std::holds_alternative<int64_t>(var)) {
    return std::to_string(std::get<int64_t>(var));
  } else if (std::holds_alternative<double>(var)) {
    return std::to_string(std::get<double>(var));
  } else if (std::holds_alternative<bool>(var)) {
    return std::get<bool>(var) ? "true" : "false";
  }
  return "";
}

/// Get string value from SoDataMap with default.
inline std::string get_string(const SoDataMap& map, const std::string& key,
                              const std::string& default_val = "") {
  auto it = map.find(key);
  if (it != map.end() && std::holds_alternative<std::string>(it->second)) {
    return std::get<std::string>(it->second);
  }
  return default_val;
}

/// Get int64_t value from SoDataMap with default.
inline int64_t get_int(const SoDataMap& map, const std::string& key, int64_t default_val = 0) {
  auto it = map.find(key);
  if (it != map.end() && std::holds_alternative<int64_t>(it->second)) {
    return std::get<int64_t>(it->second);
  }
  return default_val;
}

/// Get double value from SoDataMap with default.
inline double get_double(const SoDataMap& map, const std::string& key, double default_val = 0.0) {
  auto it = map.find(key);
  if (it != map.end() && std::holds_alternative<double>(it->second)) {
    return std::get<double>(it->second);
  }
  return default_val;
}

/// Get bool value from SoDataMap with default.
inline bool get_bool(const SoDataMap& map, const std::string& key, bool default_val = false) {
  auto it = map.find(key);
  if (it != map.end() && std::holds_alternative<bool>(it->second)) {
    return std::get<bool>(it->second);
  }
  return default_val;
}

/// Check if key exists in SoDataMap.
inline bool has_key(const SoDataMap& map, const std::string& key) {
  return map.find(key) != map.end();
}

}  // namespace so_node

}  // namespace agentflow

#endif  // AGENTFLOW_SO_NODE_H