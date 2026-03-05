// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Request wrapper for microservice internal function interface

#ifndef AGENTFLOW_NETWORK_REQUEST_H
#define AGENTFLOW_NETWORK_REQUEST_H

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace network {

/// @brief Request metadata
struct RequestMetadata {
  std::string                        request_id;
  std::string                        request_type;
  std::string                        source_id;
  std::string                        target_id;
  std::string                        workflow_id;
  std::string                        node_id;
  int64_t                            timestamp_ms;
  int                                timeout_ms;
  std::map<std::string, std::string> headers;

  RequestMetadata() : timestamp_ms(0), timeout_ms(30000) {}  // Default 30s timeout
};

/// @brief Request wrapper for microservice internal interface
class Request {
public:
  /// @brief Default constructor
  Request() = default;

  /// @brief Constructor with request type
  /// @param type The request type (e.g., "execute_workflow", "get_status")
  explicit Request(const std::string& type);

  /// @brief Full constructor
  /// @param type Request type
  /// @param payload Request payload
  /// @param metadata Request metadata
  Request(const std::string& type, const DataMap& payload, const RequestMetadata& metadata);

  /// @brief Destructor
  ~Request() = default;

  /// @brief Get request type
  const std::string& get_type() const { return type_; }

  /// @brief Set request type
  void set_type(const std::string& type) { type_ = type; }

  /// @brief Get request payload
  const DataMap& get_payload() const { return payload_; }

  /// @brief Set request payload
  void set_payload(const DataMap& payload) { payload_ = payload; }

  /// @brief Get request metadata
  const RequestMetadata& get_metadata() const { return metadata_; }

  /// @brief Set request metadata
  void set_metadata(const RequestMetadata& metadata) { metadata_ = metadata; }

  /// @brief Get request ID
  const std::string& get_request_id() const { return metadata_.request_id; }

  /// @brief Set request ID
  void set_request_id(const std::string& id) { metadata_.request_id = id; }

  /// @brief Generate a unique request ID
  static std::string generate_request_id();

  /// @brief Serialize request to JSON string
  std::string to_json() const;

  /// @brief Deserialize request from JSON string
  /// @param json_str The JSON string
  /// @return Request object or nullptr if parsing fails
  static std::shared_ptr<Request> from_json(const std::string& json_str);

  /// @brief Serialize request to binary format
  std::vector<uint8_t> to_binary() const;

  /// @brief Deserialize request from binary format
  /// @param data The binary data
  /// @param size Size of the data
  /// @return Request object or nullptr if parsing fails
  static std::shared_ptr<Request> from_binary(const uint8_t* data, size_t size);

  /// @brief Get a payload value by key
  /// @param key The key
  /// @return Value or empty variant if not found
  DataValue get(const std::string& key) const;

  /// @brief Set a payload value
  /// @param key The key
  /// @param value The value
  void set(const std::string& key, const DataValue& value);

  /// @brief Check if payload contains key
  /// @param key The key
  /// @return true if key exists
  bool has(const std::string& key) const;

  /// @brief Get a header value
  /// @param key The header key
  /// @return Header value or empty string if not found
  std::string get_header(const std::string& key) const;

  /// @brief Set a header value
  /// @param key The header key
  /// @param value The header value
  void set_header(const std::string& key, const std::string& value);

  /// @brief Validate request
  /// @return true if request is valid
  bool is_valid() const;

private:
  std::string     type_;
  DataMap         payload_;
  RequestMetadata metadata_;
};

/// @brief Request builder for convenient request construction
class RequestBuilder {
public:
  RequestBuilder();

  /// @brief Set request type
  /// @param type Request type
  /// @return Reference to builder for chaining
  RequestBuilder& with_type(const std::string& type);

  /// @brief Add payload data
  /// @param key The key
  /// @param value The value
  /// @return Reference to builder for chaining
  RequestBuilder& with_data(const std::string& key, const DataValue& value);

  /// @brief Set source ID
  /// @param source_id Source identifier
  /// @return Reference to builder for chaining
  RequestBuilder& with_source(const std::string& source_id);

  /// @brief Set target ID
  /// @param target_id Target identifier
  /// @return Reference to builder for chaining
  RequestBuilder& with_target(const std::string& target_id);

  /// @brief Set workflow ID
  /// @param workflow_id MMWorkflow identifier
  /// @return Reference to builder for chaining
  RequestBuilder& with_workflow(const std::string& workflow_id);

  /// @brief Set node ID
  /// @param node_id MMNode identifier
  /// @return Reference to builder for chaining
  RequestBuilder& with_node(const std::string& node_id);

  /// @brief Set timeout
  /// @param timeout_ms Timeout in milliseconds
  /// @return Reference to builder for chaining
  RequestBuilder& with_timeout(int timeout_ms);

  /// @brief Add header
  /// @param key Header key
  /// @param value Header value
  /// @return Reference to builder for chaining
  RequestBuilder& with_header(const std::string& key, const std::string& value);

  /// @brief Build the request
  /// @return Request object
  Request build();

  /// @brief Build and return shared pointer to request
  /// @return Shared pointer to Request object
  std::shared_ptr<Request> build_shared();

private:
  std::string     type_;
  DataMap         payload_;
  RequestMetadata metadata_;
};

/// @brief Common request types
namespace RequestTypes {
constexpr const char* EXECUTE_WORKFLOW = "execute_workflow";
constexpr const char* EXECUTE_NODE     = "execute_node";
constexpr const char* GET_STATUS       = "get_status";
constexpr const char* HEALTH_CHECK     = "health_check";
constexpr const char* GET_CONFIG       = "get_config";
constexpr const char* UPDATE_CONFIG    = "update_config";
constexpr const char* STOP_WORKFLOW    = "stop_workflow";
constexpr const char* STOP_NODE        = "stop_node";
constexpr const char* GET_METRICS      = "get_metrics";
constexpr const char* GET_LOGS         = "get_logs";
}  // namespace RequestTypes

}  // namespace network
}  // namespace agentflow

#endif  // AGENTFLOW_NETWORK_REQUEST_H
