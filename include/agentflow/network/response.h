// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Response wrapper for microservice internal function interface

#ifndef AGENTFLOW_NETWORK_RESPONSE_H
#define AGENTFLOW_NETWORK_RESPONSE_H

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "agentflow/common.h"

namespace agentflow {
namespace network {

/// @brief Response status codes
enum class ResponseStatus : int {
  OK         = 200,
  CREATED    = 201,
  ACCEPTED   = 202,
  NO_CONTENT = 204,

  BAD_REQUEST          = 400,
  UNAUTHORIZED         = 401,
  FORBIDDEN            = 403,
  NOT_FOUND            = 404,
  CONFLICT             = 409,
  UNPROCESSABLE_ENTITY = 422,

  INTERNAL_ERROR      = 500,
  NOT_IMPLEMENTED     = 501,
  BAD_GATEWAY         = 502,
  SERVICE_UNAVAILABLE = 503,
  GATEWAY_TIMEOUT     = 504,
};

/// @brief Response metadata
struct ResponseMetadata {
  std::string                        request_id;
  std::string                        response_type;
  ResponseStatus                     status;
  std::string                        status_message;
  int64_t                            timestamp_ms;
  int64_t                            processing_time_ms;
  std::map<std::string, std::string> headers;
  std::string                        error_code;
  std::string                        error_detail;

  ResponseMetadata() : status(ResponseStatus::OK), timestamp_ms(0), processing_time_ms(0) {}
};

/// @brief Response wrapper for microservice internal interface
class Response {
public:
  /// @brief Default constructor
  Response();

  /// @brief Constructor with status
  /// @param status Response status code
  explicit Response(ResponseStatus status);

  /// @brief Full constructor
  /// @param status Response status code
  /// @param payload Response payload
  /// @param metadata Response metadata
  Response(ResponseStatus status, const DataMap& payload, const ResponseMetadata& metadata);

  /// @brief Destructor
  ~Response() = default;

  /// @brief Get response status
  ResponseStatus get_status() const { return metadata_.status; }

  /// @brief Set response status
  void set_status(ResponseStatus status) { metadata_.status = status; }

  /// @brief Get response payload
  const DataMap& get_payload() const { return payload_; }

  /// @brief Set response payload
  void set_payload(const DataMap& payload) { payload_ = payload; }

  /// @brief Get response metadata
  const ResponseMetadata& get_metadata() const { return metadata_; }

  /// @brief Set response metadata
  void set_metadata(const ResponseMetadata& metadata) { metadata_ = metadata; }

  /// @brief Get request ID
  const std::string& get_request_id() const { return metadata_.request_id; }

  /// @brief Set request ID (for request-response correlation)
  void set_request_id(const std::string& id) { metadata_.request_id = id; }

  /// @brief Get status message
  const std::string& get_status_message() const { return metadata_.status_message; }

  /// @brief Set status message
  void set_status_message(const std::string& message) { metadata_.status_message = message; }

  /// @brief Get error code
  const std::string& get_error_code() const { return metadata_.error_code; }

  /// @brief Set error code
  void set_error_code(const std::string& code) { metadata_.error_code = code; }

  /// @brief Get error detail
  const std::string& get_error_detail() const { return metadata_.error_detail; }

  /// @brief Set error detail
  void set_error_detail(const std::string& detail) { metadata_.error_detail = detail; }

  /// @brief Check if response is successful (2xx status)
  bool is_success() const {
    int status = static_cast<int>(metadata_.status);
    return status >= 200 && status < 300;
  }

  /// @brief Check if response is client error (4xx status)
  bool is_client_error() const {
    int status = static_cast<int>(metadata_.status);
    return status >= 400 && status < 500;
  }

  /// @brief Check if response is server error (5xx status)
  bool is_server_error() const {
    int status = static_cast<int>(metadata_.status);
    return status >= 500 && status < 600;
  }

  /// @brief Check if response has payload data
  bool has_payload() const { return !payload_.empty(); }

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

  /// @brief Serialize response to JSON string
  std::string to_json() const;

  /// @brief Deserialize response from JSON string
  /// @param json_str The JSON string
  /// @return Response object or nullptr if parsing fails
  static std::shared_ptr<Response> from_json(const std::string& json_str);

  /// @brief Serialize response to binary format
  std::vector<uint8_t> to_binary() const;

  /// @brief Deserialize response from binary format
  /// @param data The binary data
  /// @param size Size of data
  /// @return Response object or nullptr if parsing fails
  static std::shared_ptr<Response> from_binary(const uint8_t* data, size_t size);

  /// @brief Create an OK response (200)
  /// @param payload Response payload
  /// @return OK response
  static Response ok(const DataMap& payload = DataMap());

  /// @brief Create a created response (201)
  /// @param payload Response payload
  /// @return Created response
  static Response created(const DataMap& payload = DataMap());

  /// @brief Create a bad request response (400)
  /// @param message Error message
  /// @return Bad request response
  static Response bad_request(const std::string& message = "Bad request");

  /// @brief Create a not found response (404)
  /// @param message Error message
  /// @return Not found response
  static Response not_found(const std::string& message = "Not found");

  /// @brief Create an internal error response (500)
  /// @param message Error message
  /// @return Internal error response
  static Response internal_error(const std::string& message = "Internal error");

  /// @brief Create a service unavailable response (503)
  /// @param message Error message
  /// @return Service unavailable response
  static Response service_unavailable(const std::string& message = "Service unavailable");

private:
  DataMap          payload_;
  ResponseMetadata metadata_;
};

/// @brief Response builder for convenient response construction
class ResponseBuilder {
public:
  ResponseBuilder();

  /// @brief Set response status
  /// @param status Response status
  /// @return Reference to builder for chaining
  ResponseBuilder& with_status(ResponseStatus status);

  /// @brief Add payload data
  /// @param key The key
  /// @param value The value
  /// @return Reference to builder for chaining
  ResponseBuilder& with_data(const std::string& key, const DataValue& value);

  /// @brief Set request ID (for correlation)
  /// @param request_id Request identifier
  /// @return Reference to builder for chaining
  ResponseBuilder& with_request_id(const std::string& request_id);

  /// @brief Set status message
  /// @param message Status message
  /// @return Reference to builder for chaining
  ResponseBuilder& with_status_message(const std::string& message);

  /// @brief Set error code
  /// @param code Error code
  /// @return Reference to builder for chaining
  ResponseBuilder& with_error_code(const std::string& code);

  /// @brief Set error detail
  /// @param detail Error detail
  /// @return Reference to builder for chaining
  ResponseBuilder& with_error_detail(const std::string& detail);

  /// @brief Set processing time
  /// @param time_ms Processing time in milliseconds
  /// @return Reference to builder for chaining
  ResponseBuilder& with_processing_time(int64_t time_ms);

  /// @brief Add header
  /// @param key Header key
  /// @param value Header value
  /// @return Reference to builder for chaining
  ResponseBuilder& with_header(const std::string& key, const std::string& value);

  /// @brief Build the response
  /// @return Response object
  Response build();

  /// @brief Build and return shared pointer to response
  /// @return Shared pointer to Response object
  std::shared_ptr<Response> build_shared();

private:
  ResponseStatus   status_ = ResponseStatus::OK;
  DataMap          payload_;
  ResponseMetadata metadata_;
};

}  // namespace network
}  // namespace agentflow

#endif  // AGENTFLOW_NETWORK_RESPONSE_H
