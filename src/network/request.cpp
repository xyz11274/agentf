// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Request wrapper implementation

#include "agentflow/network/request.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>

#include "agentflow/utils/logger.h"

namespace agentflow {
namespace network {

// ============================================================================
// Request Implementation
// ============================================================================

Request::Request(const std::string& type) : type_(type) {
  metadata_.request_id   = generate_request_id();
  metadata_.request_type = type;
  metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
}

Request::Request(const std::string& type, const DataMap& payload, const RequestMetadata& metadata)
    : type_(type), payload_(payload), metadata_(metadata) {
  if(metadata_.request_id.empty()) {
    metadata_.request_id = generate_request_id();
  }
  metadata_.request_type = type;
  if(metadata_.timestamp_ms == 0) {
    metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
  }
}

std::string Request::generate_request_id() {
  static std::atomic<uint64_t> counter{ 1 };
  auto                         now = std::chrono::system_clock::now();
  auto                         timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  return "req_" + std::to_string(timestamp) + "_" + std::to_string(counter.fetch_add(1));
}

std::string Request::to_json() const {
  std::ostringstream oss;

  oss << "{";
  oss << "\"type\":\"" << type_ << "\",";
  oss << "\"request_id\":\"" << metadata_.request_id << "\",";
  oss << "\"timestamp_ms\":" << metadata_.timestamp_ms << ",";

  if(!metadata_.source_id.empty()) {
    oss << "\"source_id\":\"" << metadata_.source_id << "\",";
  }

  if(!metadata_.target_id.empty()) {
    oss << "\"target_id\":\"" << metadata_.target_id << "\",";
  }

  if(!metadata_.workflow_id.empty()) {
    oss << "\"workflow_id\":\"" << metadata_.workflow_id << "\",";
  }

  if(!metadata_.node_id.empty()) {
    oss << "\"node_id\":\"" << metadata_.node_id << "\",";
  }

  oss << "\"timeout_ms\":" << metadata_.timeout_ms << ",";

  // Add payload
  oss << "\"payload\":{";
  bool first = true;
  for(const auto& [key, value] : payload_) {
    if(!first) {
      oss << ",";
    }
    first = false;

    oss << "\"" << key << "\":";

    // Serialize value based on type
    if(std::holds_alternative<int64_t>(value)) {
      oss << std::get<int64_t>(value);
    } else if(std::holds_alternative<double>(value)) {
      oss << std::get<double>(value);
    } else if(std::holds_alternative<std::string>(value)) {
      oss << "\"" << std::get<std::string>(value) << "\"";
    } else if(std::holds_alternative<bool>(value)) {
      oss << (std::get<bool>(value) ? "true" : "false");
    }
  }
  oss << "},";

  // Add headers
  oss << "\"headers\":{";
  first = true;
  for(const auto& [key, value] : metadata_.headers) {
    if(!first) {
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
  if(json_str.empty()) {
    return nullptr;
  }

  auto request = std::make_shared<Request>();

  // Simple JSON field extraction helpers
  auto extract_string = [&json_str](const std::string& key) -> std::string {
    std::string search = "\"" + key + "\":\"";
    auto        pos    = json_str.find(search);
    if(pos == std::string::npos)
      return "";
    pos += search.length();
    auto end = json_str.find('"', pos);
    if(end == std::string::npos)
      return "";
    return json_str.substr(pos, end - pos);
  };

  auto extract_int64 = [&json_str](const std::string& key) -> int64_t {
    std::string search = "\"" + key + "\":";
    auto        pos    = json_str.find(search);
    if(pos == std::string::npos)
      return 0;
    pos += search.length();
    try {
      return std::stoll(json_str.substr(pos));
    } catch(...) {
      return 0;
    }
  };

  // Extract top-level fields
  request->type_                  = extract_string("type");
  request->metadata_.request_id   = extract_string("request_id");
  request->metadata_.request_type = request->type_;
  request->metadata_.source_id    = extract_string("source_id");
  request->metadata_.target_id    = extract_string("target_id");
  request->metadata_.workflow_id  = extract_string("workflow_id");
  request->metadata_.node_id      = extract_string("node_id");
  request->metadata_.timestamp_ms = extract_int64("timestamp_ms");
  request->metadata_.timeout_ms   = static_cast<int>(extract_int64("timeout_ms"));

  // Extract payload object
  auto payload_pos = json_str.find("\"payload\":{");
  if(payload_pos != std::string::npos) {
    payload_pos += 10;  // skip "payload":{
    auto payload_end = json_str.find('}', payload_pos);
    if(payload_end != std::string::npos) {
      std::string payload_str = json_str.substr(payload_pos + 1, payload_end - payload_pos - 1);
      // Parse simple key-value pairs from payload
      size_t pos = 0;
      while(pos < payload_str.size()) {
        // Find key
        auto key_start = payload_str.find('"', pos);
        if(key_start == std::string::npos)
          break;
        auto key_end = payload_str.find('"', key_start + 1);
        if(key_end == std::string::npos)
          break;
        std::string key = payload_str.substr(key_start + 1, key_end - key_start - 1);

        // Skip ":"
        auto colon = payload_str.find(':', key_end);
        if(colon == std::string::npos)
          break;
        pos = colon + 1;

        // Skip whitespace
        while(pos < payload_str.size() && (payload_str[pos] == ' ' || payload_str[pos] == '\t'))
          pos++;

        if(pos >= payload_str.size())
          break;

        // Determine value type
        if(payload_str[pos] == '"') {
          // String value
          auto val_end = payload_str.find('"', pos + 1);
          if(val_end == std::string::npos)
            break;
          request->payload_[key] = payload_str.substr(pos + 1, val_end - pos - 1);
          pos                    = val_end + 1;
        } else if(payload_str[pos] == 't' || payload_str[pos] == 'f') {
          // Bool value
          request->payload_[key] = (payload_str[pos] == 't');
          pos += (payload_str[pos] == 't') ? 4 : 5;
        } else {
          // Numeric value
          auto val_end = payload_str.find_first_of(",}", pos);
          if(val_end == std::string::npos)
            val_end = payload_str.size();
          std::string num_str = payload_str.substr(pos, val_end - pos);
          try {
            if(num_str.find('.') != std::string::npos) {
              request->payload_[key] = std::stod(num_str);
            } else {
              request->payload_[key] = std::stoll(num_str);
            }
          } catch(...) {
            request->payload_[key] = num_str;
          }
          pos = val_end;
        }

        // Skip comma
        auto comma = payload_str.find(',', pos);
        if(comma == std::string::npos)
          break;
        pos = comma + 1;
      }
    }
  }

  // Extract headers object
  auto headers_pos = json_str.find("\"headers\":{");
  if(headers_pos != std::string::npos) {
    headers_pos += 10;
    auto headers_end = json_str.find('}', headers_pos + 1);
    if(headers_end != std::string::npos) {
      std::string headers_str = json_str.substr(headers_pos + 1, headers_end - headers_pos - 1);
      size_t      pos         = 0;
      while(pos < headers_str.size()) {
        auto key_start = headers_str.find('"', pos);
        if(key_start == std::string::npos)
          break;
        auto key_end = headers_str.find('"', key_start + 1);
        if(key_end == std::string::npos)
          break;
        std::string key = headers_str.substr(key_start + 1, key_end - key_start - 1);

        auto val_start = headers_str.find('"', key_end + 2);
        if(val_start == std::string::npos)
          break;
        auto val_end = headers_str.find('"', val_start + 1);
        if(val_end == std::string::npos)
          break;

        request->metadata_.headers[key] = headers_str.substr(val_start + 1, val_end - val_start - 1);
        pos                             = val_end + 1;
      }
    }
  }

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
  uint32_t       type_len   = static_cast<uint32_t>(type_.length());
  const uint8_t* type_bytes = reinterpret_cast<const uint8_t*>(&type_len);
  result.insert(result.end(), type_bytes, type_bytes + 4);
  result.insert(result.end(), type_.begin(), type_.end());

  // Write request_id
  const std::string& req_id           = metadata_.request_id;
  uint32_t           req_id_len       = static_cast<uint32_t>(req_id.length());
  const uint8_t*     req_id_len_bytes = reinterpret_cast<const uint8_t*>(&req_id_len);
  result.insert(result.end(), req_id_len_bytes, req_id_len_bytes + 4);
  result.insert(result.end(), req_id.begin(), req_id.end());

  // Write timestamp
  const uint8_t* timestamp_bytes = reinterpret_cast<const uint8_t*>(&metadata_.timestamp_ms);
  result.insert(result.end(), timestamp_bytes, timestamp_bytes + 8);

  // Write timeout
  const uint8_t* timeout_bytes = reinterpret_cast<const uint8_t*>(&metadata_.timeout_ms);
  result.insert(result.end(), timeout_bytes, timeout_bytes + 4);

  // Write payload count
  uint32_t       payload_count       = static_cast<uint32_t>(payload_.size());
  const uint8_t* payload_count_bytes = reinterpret_cast<const uint8_t*>(&payload_count);
  result.insert(result.end(), payload_count_bytes, payload_count_bytes + 4);

  // Write payload entries (simplified - would need proper serialization)
  // For now, skip detailed payload serialization

  // Write headers count
  uint32_t       headers_count       = static_cast<uint32_t>(metadata_.headers.size());
  const uint8_t* headers_count_bytes = reinterpret_cast<const uint8_t*>(&headers_count);
  result.insert(result.end(), headers_count_bytes, headers_count_bytes + 4);

  // Write headers
  for(const auto& [key, value] : metadata_.headers) {
    uint32_t       key_len       = static_cast<uint32_t>(key.length());
    const uint8_t* key_len_bytes = reinterpret_cast<const uint8_t*>(&key_len);
    result.insert(result.end(), key_len_bytes, key_len_bytes + 4);
    result.insert(result.end(), key.begin(), key.end());

    uint32_t       value_len       = static_cast<uint32_t>(value.length());
    const uint8_t* value_len_bytes = reinterpret_cast<const uint8_t*>(&value_len);
    result.insert(result.end(), value_len_bytes, value_len_bytes + 4);
    result.insert(result.end(), value.begin(), value.end());
  }

  return result;
}

std::shared_ptr<Request> Request::from_binary(const uint8_t* data, size_t size) {
  if(!data || size < 24) {  // minimum: type_len(4) + req_id_len(4) + timestamp(8) + timeout(4) + payload_cnt(4)
    return nullptr;
  }

  auto   request = std::make_shared<Request>();
  size_t offset  = 0;

  auto read_uint32 = [&](uint32_t& val) -> bool {
    if(offset + 4 > size)
      return false;
    std::memcpy(&val, data + offset, 4);
    offset += 4;
    return true;
  };

  auto read_string = [&](std::string& str, uint32_t len) -> bool {
    if(offset + len > size)
      return false;
    str.assign(reinterpret_cast<const char*>(data + offset), len);
    offset += len;
    return true;
  };

  // Read type
  uint32_t type_len = 0;
  if(!read_uint32(type_len) || offset + type_len > size)
    return nullptr;
  read_string(request->type_, type_len);
  request->metadata_.request_type = request->type_;

  // Read request_id
  uint32_t req_id_len = 0;
  if(!read_uint32(req_id_len) || offset + req_id_len > size)
    return nullptr;
  read_string(request->metadata_.request_id, req_id_len);

  // Read timestamp_ms (8 bytes)
  if(offset + 8 > size)
    return nullptr;
  std::memcpy(&request->metadata_.timestamp_ms, data + offset, 8);
  offset += 8;

  // Read timeout_ms (4 bytes — note: to_binary writes 4 bytes for int)
  if(offset + 4 > size)
    return nullptr;
  std::memcpy(&request->metadata_.timeout_ms, data + offset, 4);
  offset += 4;

  // Read payload count (skip payload entries as to_binary also skips them)
  uint32_t payload_count = 0;
  if(!read_uint32(payload_count))
    return nullptr;

  // Read headers count
  uint32_t headers_count = 0;
  if(!read_uint32(headers_count))
    return nullptr;

  // Read headers
  for(uint32_t i = 0; i < headers_count; ++i) {
    uint32_t    key_len = 0;
    std::string key, value;
    if(!read_uint32(key_len))
      return nullptr;
    if(!read_string(key, key_len))
      return nullptr;

    uint32_t value_len = 0;
    if(!read_uint32(value_len))
      return nullptr;
    if(!read_string(value, value_len))
      return nullptr;

    request->metadata_.headers[key] = value;
  }

  return request;
}

DataValue Request::get(const std::string& key) const {
  auto it = payload_.find(key);
  if(it != payload_.end()) {
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
  if(it != metadata_.headers.end()) {
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
  metadata_.request_id   = Request::generate_request_id();
  metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  metadata_.timeout_ms = 30000;  // Default 30s
}

RequestBuilder& RequestBuilder::with_type(const std::string& type) {
  type_                  = type;
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
