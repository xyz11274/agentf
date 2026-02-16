// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Request wrapper implementation

#include "agentflow/network/request.h"
#include "agentflow/basic/logger.h"
#include <chrono>
#include <sstream>
#include <atomic>

namespace agentflow {
namespace network {

// ============================================================================
// Request Implementation
// ============================================================================

Request::Request(const std::string& type)
    : type_(type) {
  metadata_.request_id = generate_request_id();
  metadata_.request_type = type;
  metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

Request::Request(const std::string& type, const DataMap& payload,
               const RequestMetadata& metadata)
    : type_(type), payload_(payload), metadata_(metadata) {
  if (metadata_.request_id.empty()) {
    metadata_.request_id = generate_request_id();
  }
  metadata_.request_type = type;
  if (metadata_.timestamp_ms == 0) {
    metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }
}

std::string Request::generate_request_id() {
  static std::atomic<uint64_t> counter{1};
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
  return "req_" + std::to_string(timestamp) + "_" +
         std::to_string(counter.fetch_add(1));
}

std::string Request::to_json() const {
  std::ostringstream oss;

  oss << "{";
  oss << "\"type\":\"" << type_ << "\",";
  oss << "\"request_id\":\"" << metadata_.request_id << "\",";
  oss << "\"timestamp_ms\":" << metadata_.timestamp_ms << ",";

  if (!metadata_.source_id.empty()) {
    oss << "\"source_id\":\"" << metadata_.source_id << "\",";
  }

  if (!metadata_.target_id.empty()) {
    oss << "\"target_id\":\"" << metadata_.target_id << "\",";
  }

  if (!metadata_.workflow_id.empty()) {
    oss << "\"workflow_id\":\"" << metadata_.workflow_id << "\",";
  }

  if (!metadata_.node_id.empty()) {
    oss << "\"node_id\":\"" << metadata_.node_id << "\",";
  }

  oss << "\"timeout_ms\":" << metadata_.timeout_ms << ",";

  // Add payload
  oss << "\"payload\":{";
  bool first = true;
  for (const auto& [key, value] : payload_) {
    if (!first) {
      oss << ",";
    }
    first = false;

    oss << "\"" << key << "\":";

    // Serialize value based on type
    if (std::holds_alternative<int64_t>(value)) {
      oss << std::get<int64_t>(value);
    } else if (std::holds_alternative<double>(value)) {
      oss << std::get<double>(value);
    } else if (std::holds_alternative<std::string>(value)) {
      oss << "\"" << std::get<std::string>(value) << "\"";
    } else if (std::holds_alternative<bool>(value)) {
      oss << (std::get<bool>(value) ? "true" : "false");
    }
  }
  oss << "},";

  // Add headers
  oss << "\"headers\":{";
  first = true;
  for (const auto& [key, value] : metadata_.headers) {
    if (!first) {
      oss << ",";
    }
    first = false;
    oss << "\"" << key << "\":\"" << value << "\"";
  }
  oss << "}";

  oss << "}";

  return oss.str();
}

std::shared_ptr<Request> Request::from_json(const std::string& json_str) {
  // Placeholder: JSON parsing would require a JSON library
  // This is a simplified implementation
  Logger::instance().warn("Request::from_json not fully implemented");
  auto request = std::make_shared<Request>("unknown");
  return request;
}

std::vector<uint8_t> Request::to_binary() const {
  std::vector<uint8_t> result;

  // Binary format:
  // [4 bytes] type_length
  // [N bytes] type
  // [4 bytes] request_id_length
  // [N bytes] request_id
  // [8 bytes] timestamp_ms
  // [8 bytes] timeout_ms
  // [4 bytes] payload_count
  // For each payload entry:
  //   [4 bytes] key_length
  //   [N bytes] key
  //   [1 byte] value_type
  //   [4 bytes] value_length
  //   [N bytes] value
  // [4 bytes] headers_count
  // For each header:
  //   [4 bytes] key_length
  //   [N bytes] key
  //   [4 bytes] value_length
  //   [N bytes] value

  // Write type
  uint32_t type_len = static_cast<uint32_t>(type_.length());
  const uint8_t* type_bytes = reinterpret_cast<const uint8_t*>(&type_len);
  result.insert(result.end(), type_bytes, type_bytes + 4);
  result.insert(result.end(), type_.begin(), type_.end());

  // Write request_id
  const std::string& req_id = metadata_.request_id;
  uint32_t req_id_len = static_cast<uint32_t>(req_id.length());
  const uint8_t* req_id_len_bytes = reinterpret_cast<const uint8_t*>(&req_id_len);
  result.insert(result.end(), req_id_len_bytes, req_id_len_bytes + 4);
  result.insert(result.end(), req_id.begin(), req_id.end());

  // Write timestamp
  const uint8_t* timestamp_bytes = reinterpret_cast<const uint8_t*>(&metadata_.timestamp_ms);
  result.insert(result.end(), timestamp_bytes, timestamp_bytes + 8);

  // Write timeout
  const uint8_t* timeout_bytes = reinterpret_cast<const uint8_t*>(&metadata_.timeout_ms);
  result.insert(result.end(), timeout_bytes, timeout_bytes + 4);

  // Write payload count
  uint32_t payload_count = static_cast<uint32_t>(payload_.size());
  const uint8_t* payload_count_bytes = reinterpret_cast<const uint8_t*>(&payload_count);
  result.insert(result.end(), payload_count_bytes, payload_count_bytes + 4);

  // Write payload entries (simplified - would need proper serialization)
  // For now, skip detailed payload serialization

  // Write headers count
  uint32_t headers_count = static_cast<uint32_t>(metadata_.headers.size());
  const uint8_t* headers_count_bytes = reinterpret_cast<const uint8_t*>(&headers_count);
  result.insert(result.end(), headers_count_bytes, headers_count_bytes + 4);

  // Write headers
  for (const auto& [key, value] : metadata_.headers) {
    uint32_t key_len = static_cast<uint32_t>(key.length());
    const uint8_t* key_len_bytes = reinterpret_cast<const uint8_t*>(&key_len);
    result.insert(result.end(), key_len_bytes, key_len_bytes + 4);
    result.insert(result.end(), key.begin(), key.end());

    uint32_t value_len = static_cast<uint32_t>(value.length());
    const uint8_t* value_len_bytes = reinterpret_cast<const uint8_t*>(&value_len);
    result.insert(result.end(), value_len_bytes, value_len_bytes + 4);
    result.insert(result.end(), value.begin(), value.end());
  }

  return result;
}

std::shared_ptr<Request> Request::from_binary(const uint8_t* data, size_t size) {
  // Placeholder: Binary deserialization would need full implementation
  Logger::instance().warn("Request::from_binary not fully implemented");
  auto request = std::make_shared<Request>("unknown");
  return request;
}

DataValue Request::get(const std::string& key) const {
  auto it = payload_.find(key);
  if (it != payload_.end()) {
    return it->second;
  }
  return std::string();
}

void Request::set(const std::string& key, const DataValue& value) {
  payload_[key] = value;
}

bool Request::has(const std::string& key) const {
  return payload_.find(key) != payload_.end();
}

std::string Request::get_header(const std::string& key) const {
  auto it = metadata_.headers.find(key);
  if (it != metadata_.headers.end()) {
    return it->second;
  }
  return "";
}

void Request::set_header(const std::string& key, const std::string& value) {
  metadata_.headers[key] = value;
}

bool Request::is_valid() const {
  return !type_.empty() && !metadata_.request_id.empty();
}

// ============================================================================
// RequestBuilder Implementation
// ============================================================================

RequestBuilder::RequestBuilder() {
  metadata_.request_id = Request::generate_request_id();
  metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  metadata_.timeout_ms = 30000;  // Default 30s
}

RequestBuilder& RequestBuilder::with_type(const std::string& type) {
  type_ = type;
  metadata_.request_type = type;
  return *this;
}

RequestBuilder& RequestBuilder::with_data(const std::string& key, const DataValue& value) {
  payload_[key] = value;
  return *this;
}

RequestBuilder& RequestBuilder::with_source(const std::string& source_id) {
  metadata_.source_id = source_id;
  return *this;
}

RequestBuilder& RequestBuilder::with_target(const std::string& target_id) {
  metadata_.target_id = target_id;
  return *this;
}

RequestBuilder& RequestBuilder::with_workflow(const std::string& workflow_id) {
  metadata_.workflow_id = workflow_id;
  return *this;
}

RequestBuilder& RequestBuilder::with_node(const std::string& node_id) {
  metadata_.node_id = node_id;
  return *this;
}

RequestBuilder& RequestBuilder::with_timeout(int timeout_ms) {
  metadata_.timeout_ms = timeout_ms;
  return *this;
}

RequestBuilder& RequestBuilder::with_header(const std::string& key, const std::string& value) {
  metadata_.headers[key] = value;
  return *this;
}

Request RequestBuilder::build() {
  return Request(type_, payload_, metadata_);
}

std::shared_ptr<Request> RequestBuilder::build_shared() {
  return std::make_shared<Request>(type_, payload_, metadata_);
}

}  // namespace network
}  // namespace agentflow
