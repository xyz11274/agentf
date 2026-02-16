// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Private protocol implementation

#include "agentflow/network/private_protocol.h"
#include "agentflow/basic/logger.h"

#include <cstring>
#include <algorithm>
#include <string>

namespace agentflow {
namespace network {

// ============================================================================
// Protocol Encoder Implementation
// ============================================================================

std::vector<uint8_t> ProtocolEncoder::encode_header(const ProtocolHeader& header) {
  std::vector<uint8_t> buffer(sizeof(ProtocolHeader));
  memcpy(buffer.data(), &header, sizeof(ProtocolHeader));
  return buffer;
}

template<typename T>
std::vector<uint8_t> ProtocolEncoder::encode_message(const T& message) {
  // This is a simplified encoder - in real implementation,
  // you would use proper serialization
  std::vector<uint8_t> buffer;

  // Copy message data to buffer
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&message);
  size_t size = sizeof(T);
  buffer.assign(data, data + size);

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_data_request(const DataRequest& request) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(request);
  header.set_header(static_cast<uint16_t>(MessageType::DATA_REQUEST),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_data_response(const DataResponse& response) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(response);
  header.set_header(static_cast<uint16_t>(MessageType::DATA_RESPONSE),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_control_start(const ControlStart& start) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(start);
  header.set_header(static_cast<uint16_t>(MessageType::CONTROL_START),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_control_status(const ControlStatus& status) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(status);
  header.set_header(static_cast<uint16_t>(MessageType::CONTROL_STATUS),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_flow_condition(const FlowCondition& condition) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(condition);
  header.set_header(static_cast<uint16_t>(MessageType::FLOW_CONDITION),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_flow_loop(const FlowLoop& loop) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(loop);
  header.set_header(static_cast<uint16_t>(MessageType::FLOW_LOOP),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_flow_exception(const FlowException& exception) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(exception);
  header.set_header(static_cast<uint16_t>(MessageType::FLOW_EXCEPTION),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

std::vector<uint8_t> ProtocolEncoder::encode_error(const ErrorMessage& error) {
  // Create header
  ProtocolHeader header;
  std::vector<uint8_t> body = encode_message(error);
  header.set_header(static_cast<uint16_t>(MessageType::ERROR_INTERNAL),
               static_cast<uint32_t>(body.size()));

  // Combine header and body
  std::vector<uint8_t> buffer = encode_header(header);
  buffer.insert(buffer.end(), body.begin(), body.end());

  return buffer;
}

// ============================================================================
// Protocol Decoder Implementation
// ============================================================================

bool ProtocolDecoder::decode_header(const uint8_t* data, size_t size, ProtocolHeader& header) {
  if (size < sizeof(ProtocolHeader)) {
    std::string msg = "Insufficient data for header: need " +
                     std::to_string(sizeof(ProtocolHeader)) + " bytes, got " +
                     std::to_string(size);
    Logger::instance().error(msg);
    return false;
  }

  memcpy(&header, data, sizeof(ProtocolHeader));

  if (!header.is_valid()) {
    std::string msg = "Invalid protocol header: magic=0x" +
                     std::to_string(header.magic);
    Logger::instance().error(msg);
    return false;
  }

  return true;
}

template<typename T>
bool ProtocolDecoder::decode_message(const uint8_t* data, size_t size, T& message) {
  if (size < sizeof(T)) {
    Logger::instance().error("Insufficient data for message: expected " + std::to_string(sizeof(T)) + " bytes");
    return false;
  }

  // Simplified decoding - copy data directly
  memcpy(&message, data, sizeof(T));
  return true;
}

// Explicit template instantiations for decode_message
template bool ProtocolDecoder::decode_message<DataRequest>(const uint8_t*, size_t, DataRequest&);
template bool ProtocolDecoder::decode_message<DataResponse>(const uint8_t*, size_t, DataResponse&);
template bool ProtocolDecoder::decode_message<ControlStart>(const uint8_t*, size_t, ControlStart&);
template bool ProtocolDecoder::decode_message<ControlStatus>(const uint8_t*, size_t, ControlStatus&);
template bool ProtocolDecoder::decode_message<FlowCondition>(const uint8_t*, size_t, FlowCondition&);
template bool ProtocolDecoder::decode_message<FlowLoop>(const uint8_t*, size_t, FlowLoop&);
template bool ProtocolDecoder::decode_message<FlowException>(const uint8_t*, size_t, FlowException&);
template bool ProtocolDecoder::decode_message<ErrorMessage>(const uint8_t*, size_t, ErrorMessage&);

}  // namespace network
}  // namespace agentflow
