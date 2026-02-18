// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Response wrapper implementation

#include "network/response.h"

#include <chrono>
#include <cstring>
#include <sstream>

#include "utils/logger.h"

namespace agentflow {
namespace network {

// ============================================================================
// Response Implementation
// ============================================================================

Response::Response() : payload_() {
  metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
}

Response::Response(ResponseStatus status) : payload_() {
  metadata_.status       = status;
  metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();

  // Set default status message based on status
  switch(static_cast<int>(status)) {
    case 200:
      metadata_.status_message = "OK";
      break;
    case 201:
      metadata_.status_message = "Created";
      break;
    case 400:
      metadata_.status_message = "Bad Request";
      break;
    case 404:
      metadata_.status_message = "Not Found";
      break;
    case 500:
      metadata_.status_message = "Internal Server Error";
      break;
    case 503:
      metadata_.status_message = "Service Unavailable";
      break;
    default:
      metadata_.status_message = "Unknown Status";
      break;
  }
}

Response::Response(ResponseStatus status, const DataMap& payload, const ResponseMetadata& metadata)
    : payload_(payload), metadata_(metadata) {
  metadata_.status = status;
  if(metadata_.timestamp_ms == 0) {
    metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
  }
}

DataValue Response::get(const std::string& key) const {
  auto it = payload_.find(key);
  if(it != payload_.end()) {
    return it->second;
  }
  return std::string();
}

void Response::set(const std::string& key, const DataValue& value) {
  payload_[key] = value;
}

bool Response::has(const std::string& key) const {
  return payload_.find(key) != payload_.end();
}

std::string Response::get_header(const std::string& key) const {
  auto it = metadata_.headers.find(key);
  if(it != metadata_.headers.end()) {
    return it->second;
  }
  return "";
}

void Response::set_header(const std::string& key, const std::string& value) {
  metadata_.headers[key] = value;
}

std::string Response::to_json() const {
  std::ostringstream oss;

  oss << "{";
  oss << "\"status\":" << static_cast<int>(metadata_.status) << ",";
  oss << "\"status_message\":\"" << metadata_.status_message << "\",";
  oss << "\"timestamp_ms\":" << metadata_.timestamp_ms << ",";
  oss << "\"processing_time_ms\":" << metadata_.processing_time_ms << ",";

  if(!metadata_.request_id.empty()) {
    oss << "\"request_id\":\"" << metadata_.request_id << "\",";
  }

  if(!metadata_.error_code.empty()) {
    oss << "\"error_code\":\"" << metadata_.error_code << "\",";
  }

  if(!metadata_.error_detail.empty()) {
    oss << "\"error_detail\":\"" << metadata_.error_detail << "\",";
  }

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

std::shared_ptr<Response> Response::from_json(const std::string& json_str) {
  if(json_str.empty()) {
    return nullptr;
  }

  auto response = std::make_shared<Response>();

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

  // Extract fields
  response->metadata_.status =
      static_cast<ResponseStatus>(static_cast<int>(extract_int64("status")));
  response->metadata_.status_message    = extract_string("status_message");
  response->metadata_.timestamp_ms      = extract_int64("timestamp_ms");
  response->metadata_.processing_time_ms = extract_int64("processing_time_ms");
  response->metadata_.request_id        = extract_string("request_id");
  response->metadata_.error_code        = extract_string("error_code");
  response->metadata_.error_detail      = extract_string("error_detail");

  // Extract payload object
  auto payload_pos = json_str.find("\"payload\":{");
  if(payload_pos != std::string::npos) {
    payload_pos += 10;
    auto payload_end = json_str.find('}', payload_pos + 1);
    if(payload_end != std::string::npos) {
      std::string payload_str = json_str.substr(payload_pos + 1, payload_end - payload_pos - 1);
      size_t      pos         = 0;
      while(pos < payload_str.size()) {
        auto key_start = payload_str.find('"', pos);
        if(key_start == std::string::npos)
          break;
        auto key_end = payload_str.find('"', key_start + 1);
        if(key_end == std::string::npos)
          break;
        std::string key = payload_str.substr(key_start + 1, key_end - key_start - 1);

        auto colon = payload_str.find(':', key_end);
        if(colon == std::string::npos)
          break;
        pos = colon + 1;
        while(pos < payload_str.size() && payload_str[pos] == ' ')
          pos++;
        if(pos >= payload_str.size())
          break;

        if(payload_str[pos] == '"') {
          auto val_end = payload_str.find('"', pos + 1);
          if(val_end == std::string::npos)
            break;
          response->payload_[key] = payload_str.substr(pos + 1, val_end - pos - 1);
          pos                     = val_end + 1;
        } else if(payload_str[pos] == 't' || payload_str[pos] == 'f') {
          response->payload_[key] = (payload_str[pos] == 't');
          pos += (payload_str[pos] == 't') ? 4 : 5;
        } else {
          auto        val_end = payload_str.find_first_of(",}", pos);
          if(val_end == std::string::npos)
            val_end = payload_str.size();
          std::string num_str = payload_str.substr(pos, val_end - pos);
          try {
            if(num_str.find('.') != std::string::npos) {
              response->payload_[key] = std::stod(num_str);
            } else {
              response->payload_[key] = std::stoll(num_str);
            }
          } catch(...) {
            response->payload_[key] = num_str;
          }
          pos = val_end;
        }
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
        std::string key       = headers_str.substr(key_start + 1, key_end - key_start - 1);
        auto        val_start = headers_str.find('"', key_end + 2);
        if(val_start == std::string::npos)
          break;
        auto val_end = headers_str.find('"', val_start + 1);
        if(val_end == std::string::npos)
          break;
        response->metadata_.headers[key] = headers_str.substr(val_start + 1, val_end - val_start - 1);
        pos                              = val_end + 1;
      }
    }
  }

  return response;
}

std::vector<uint8_t> Response::to_binary() const {
  std::vector<uint8_t> result;

  // Binary format:
  // [4 bytes] status
  // [4 bytes] status_message_length
  // [N bytes] status_message
  // [4 bytes] request_id_length
  // [N bytes] request_id
  // [8 bytes] timestamp_ms
  // [8 bytes] processing_time_ms
  // [4 bytes] error_code_length
  // [N bytes] error_code
  // [4 bytes] error_detail_length
  // [N bytes] error_detail
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

  // Write status
  int            status       = static_cast<int>(metadata_.status);
  const uint8_t* status_bytes = reinterpret_cast<const uint8_t*>(&status);
  result.insert(result.end(), status_bytes, status_bytes + 4);

  // Write status message
  const std::string& msg           = metadata_.status_message;
  uint32_t           msg_len       = static_cast<uint32_t>(msg.length());
  const uint8_t*     msg_len_bytes = reinterpret_cast<const uint8_t*>(&msg_len);
  result.insert(result.end(), msg_len_bytes, msg_len_bytes + 4);
  result.insert(result.end(), msg.begin(), msg.end());

  // Write request_id
  const std::string& req_id           = metadata_.request_id;
  uint32_t           req_id_len       = static_cast<uint32_t>(req_id.length());
  const uint8_t*     req_id_len_bytes = reinterpret_cast<const uint8_t*>(&req_id_len);
  result.insert(result.end(), req_id_len_bytes, req_id_len_bytes + 4);
  result.insert(result.end(), req_id.begin(), req_id.end());

  // Write timestamp
  const uint8_t* timestamp_bytes = reinterpret_cast<const uint8_t*>(&metadata_.timestamp_ms);
  result.insert(result.end(), timestamp_bytes, timestamp_bytes + 8);

  // Write processing time
  const uint8_t* processing_time_bytes =
      reinterpret_cast<const uint8_t*>(&metadata_.processing_time_ms);
  result.insert(result.end(), processing_time_bytes, processing_time_bytes + 4);

  // Write error code
  const std::string& error_code           = metadata_.error_code;
  uint32_t           error_code_len       = static_cast<uint32_t>(error_code.length());
  const uint8_t*     error_code_len_bytes = reinterpret_cast<const uint8_t*>(&error_code_len);
  result.insert(result.end(), error_code_len_bytes, error_code_len_bytes + 4);
  result.insert(result.end(), error_code.begin(), error_code.end());

  // Write error detail
  const std::string& error_detail           = metadata_.error_detail;
  uint32_t           error_detail_len       = static_cast<uint32_t>(error_detail.length());
  const uint8_t*     error_detail_len_bytes = reinterpret_cast<const uint8_t*>(&error_detail_len);
  result.insert(result.end(), error_detail_len_bytes, error_detail_len_bytes + 4);
  result.insert(result.end(), error_detail.begin(), error_detail.end());

  // Write payload count
  uint32_t       payload_count       = static_cast<uint32_t>(payload_.size());
  const uint8_t* payload_count_bytes = reinterpret_cast<const uint8_t*>(&payload_count);
  result.insert(result.end(), payload_count_bytes, payload_count_bytes + 4);

  // Write payload entries (simplified)
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

std::shared_ptr<Response> Response::from_binary(const uint8_t* data, size_t size) {
  if(!data || size < 20) {
    return nullptr;
  }

  auto   response = std::make_shared<Response>();
  size_t offset   = 0;

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

  // Read status (4 bytes int)
  int status_int = 0;
  if(offset + 4 > size)
    return nullptr;
  std::memcpy(&status_int, data + offset, 4);
  offset += 4;
  response->metadata_.status = static_cast<ResponseStatus>(status_int);

  // Read status_message
  uint32_t msg_len = 0;
  if(!read_uint32(msg_len) || offset + msg_len > size)
    return nullptr;
  read_string(response->metadata_.status_message, msg_len);

  // Read request_id
  uint32_t req_id_len = 0;
  if(!read_uint32(req_id_len) || offset + req_id_len > size)
    return nullptr;
  read_string(response->metadata_.request_id, req_id_len);

  // Read timestamp_ms (8 bytes)
  if(offset + 8 > size)
    return nullptr;
  std::memcpy(&response->metadata_.timestamp_ms, data + offset, 8);
  offset += 8;

  // Read processing_time_ms (4 bytes — to_binary writes 4 bytes)
  if(offset + 4 > size)
    return nullptr;
  int32_t proc_time = 0;
  std::memcpy(&proc_time, data + offset, 4);
  response->metadata_.processing_time_ms = proc_time;
  offset += 4;

  // Read error_code
  uint32_t error_code_len = 0;
  if(!read_uint32(error_code_len) || offset + error_code_len > size)
    return nullptr;
  read_string(response->metadata_.error_code, error_code_len);

  // Read error_detail
  uint32_t error_detail_len = 0;
  if(!read_uint32(error_detail_len) || offset + error_detail_len > size)
    return nullptr;
  read_string(response->metadata_.error_detail, error_detail_len);

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
    response->metadata_.headers[key] = value;
  }

  return response;
}

Response Response::ok(const DataMap& payload) {
  Response response(ResponseStatus::OK);
  response.set_payload(payload);
  return response;
}

Response Response::created(const DataMap& payload) {
  Response response(ResponseStatus::CREATED);
  response.set_payload(payload);
  return response;
}

Response Response::bad_request(const std::string& message) {
  Response response(ResponseStatus::BAD_REQUEST);
  response.set_status_message(message);
  return response;
}

Response Response::not_found(const std::string& message) {
  Response response(ResponseStatus::NOT_FOUND);
  response.set_status_message(message);
  return response;
}

Response Response::internal_error(const std::string& message) {
  Response response(ResponseStatus::INTERNAL_ERROR);
  response.set_status_message(message);
  return response;
}

Response Response::service_unavailable(const std::string& message) {
  Response response(ResponseStatus::SERVICE_UNAVAILABLE);
  response.set_status_message(message);
  return response;
}

// ============================================================================
// ResponseBuilder Implementation
// ============================================================================

ResponseBuilder::ResponseBuilder() : payload_() {
  metadata_.status       = ResponseStatus::OK;
  metadata_.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  metadata_.status_message = "OK";
}

ResponseBuilder& ResponseBuilder::with_status(ResponseStatus status) {
  status_          = status;
  metadata_.status = status;
  return *this;
}

ResponseBuilder& ResponseBuilder::with_data(const std::string& key, const DataValue& value) {
  payload_[key] = value;
  return *this;
}

ResponseBuilder& ResponseBuilder::with_request_id(const std::string& request_id) {
  metadata_.request_id = request_id;
  return *this;
}

ResponseBuilder& ResponseBuilder::with_status_message(const std::string& message) {
  metadata_.status_message = message;
  return *this;
}

ResponseBuilder& ResponseBuilder::with_error_code(const std::string& code) {
  metadata_.error_code = code;
  return *this;
}

ResponseBuilder& ResponseBuilder::with_error_detail(const std::string& detail) {
  metadata_.error_detail = detail;
  return *this;
}

ResponseBuilder& ResponseBuilder::with_processing_time(int64_t time_ms) {
  metadata_.processing_time_ms = time_ms;
  return *this;
}

ResponseBuilder& ResponseBuilder::with_header(const std::string& key, const std::string& value) {
  metadata_.headers[key] = value;
  return *this;
}

Response ResponseBuilder::build() {
  return Response(status_, payload_, metadata_);
}

std::shared_ptr<Response> ResponseBuilder::build_shared() {
  return std::make_shared<Response>(status_, payload_, metadata_);
}

}  // namespace network
}  // namespace agentflow
