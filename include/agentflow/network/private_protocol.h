// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Private protocol for business data communication

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <map>

namespace agentflow {
namespace network {

/**
 * Private protocol version
 */
constexpr uint32_t PRIVATE_PROTOCOL_VERSION = 0x0001;

/**
 * Protocol header format (24 bytes total)
 */
struct ProtocolHeader {
  uint32_t magic;           // Magic number: 0xAF01AF01
  uint16_t version;          // Protocol version
  uint16_t message_type;    // Message type
  uint32_t body_length;      // Message body length
  uint32_t sequence_id;     // Message sequence ID
  uint32_t flags;           // Flags (bit fields)
  uint64_t timestamp;        // Timestamp in nanoseconds

  // Initialize header
  ProtocolHeader() { std::memset(this, 0, sizeof(*this)); }

  // Set header fields
  void set_header(uint16_t msg_type, uint32_t body_len, uint32_t seq_id = 0) {
    magic = 0xAF01AF01;
    version = PRIVATE_PROTOCOL_VERSION;
    message_type = msg_type;
    body_length = body_len;
    sequence_id = seq_id;
    timestamp = get_timestamp_ns();
    flags = 0;
  }

  bool is_valid() const {
    return magic == 0xAF01AF01;
  }

 private:
  static uint64_t get_timestamp_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }
};

/**
 * Message types
 */
enum class MessageType : uint16_t {
  // Data messages
  DATA_REQUEST = 0x0001,
  DATA_RESPONSE = 0x0002,
  DATA_BROADCAST = 0x0003,
  DATA_SUBSCRIBE = 0x0004,
  DATA_UNSUBSCRIBE = 0x0005,

  // Control messages
  CONTROL_START = 0x0101,
  CONTROL_STOP = 0x0102,
  CONTROL_PAUSE = 0x0103,
  CONTROL_RESUME = 0x0104,
  CONTROL_STATUS = 0x0105,

  // Flow control messages
  FLOW_CONDITION = 0x0201,
  FLOW_LOOP = 0x0202,
  FLOW_BRANCH = 0x0203,
  FLOW_EXCEPTION = 0x0204,

  // Connection messages
  CONN_CONNECT = 0x0301,
  CONN_DISCONNECT = 0x0302,
  CONN_PING = 0x0303,
  CONN_PONG = 0x0304,

  // Error messages
  ERROR_PROTOCOL = 0x0401,
  ERROR_VERSION = 0x0402,
  ERROR_CHECKSUM = 0x0403,
  ERROR_INTERNAL = 0x0404,
};

/**
 * Protocol flags
 */
enum class ProtocolFlags : uint32_t {
  NONE = 0x0000,
  COMPRESSED = 0x0001,        // Body is compressed
  ENCRYPTED = 0x0002,         // Body is encrypted
  STREAMING = 0x0004,         // Streaming data
  BROADCAST = 0x0008,          // Broadcast message
  NO_ACK = 0x0010,            // No acknowledgment needed
};

/**
 * Data request message
 */
struct DataRequest {
  std::string sender_id;
  std::string receiver_id;
  std::string data_key;
  std::string request_id;
};

/**
 * Data response message
 */
struct DataResponse {
  std::string sender_id;
  std::string receiver_id;
  std::string data_key;
  std::string request_id;
  bool success;
  std::string error_message;
  std::vector<uint8_t> data;  // Variable length data
};

/**
 * Control start message
 */
struct ControlStart {
  std::string controller_id;
  std::string target_id;
  std::string control_type;
};

/**
 * Control status message
 */
struct ControlStatus {
  std::string controller_id;
  std::string target_id;
  std::string status;
  std::map<std::string, std::string> metadata;
};

/**
 * Flow condition message
 */
struct FlowCondition {
  std::string workflow_id;
  std::string node_id;
  std::string condition_key;
  std::string condition_value;
  bool condition_result;
};

/**
 * Flow loop message
 */
struct FlowLoop {
  std::string workflow_id;
  std::string loop_id;
  std::string loop_type;
  int loop_iteration;
  bool continue_loop;
};

/**
 * Flow exception message
 */
struct FlowException {
  std::string workflow_id;
  std::string node_id;
  std::string exception_type;
  std::string exception_message;
  bool should_retry;
  bool should_rollback;
};

/**
 * Error message
 */
struct ErrorMessage {
  std::string sender_id;
  std::string receiver_id;
  uint16_t error_code;
  std::string error_message;
  std::map<std::string, std::string> context;
};

/**
 * Protocol encoder
 */
class ProtocolEncoder {
 public:
  /**
   * Encode message to byte buffer
   */
  static std::vector<uint8_t> encode_header(const ProtocolHeader& header);

  template<typename T>
  static std::vector<uint8_t> encode_message(const T& message);

  /**
   * Encode data request
   */
  static std::vector<uint8_t> encode_data_request(const DataRequest& request);

  /**
   * Encode data response
   */
  static std::vector<uint8_t> encode_data_response(const DataResponse& response);

  /**
   * Encode control start
   */
  static std::vector<uint8_t> encode_control_start(const ControlStart& start);

  /**
   * Encode control status
   */
  static std::vector<uint8_t> encode_control_status(const ControlStatus& status);

  /**
   * Encode flow condition
   */
  static std::vector<uint8_t> encode_flow_condition(const FlowCondition& condition);

  /**
   * Encode flow loop
   */
  static std::vector<uint8_t> encode_flow_loop(const FlowLoop& loop);

  /**
   * Encode flow exception
   */
  static std::vector<uint8_t> encode_flow_exception(const FlowException& exception);

  /**
   * Encode error message
   */
  static std::vector<uint8_t> encode_error(const ErrorMessage& error);
};

/**
 * Protocol decoder
 */
class ProtocolDecoder {
 public:
  /**
   * Decode header from byte buffer
   */
  static bool decode_header(const uint8_t* data, size_t size, ProtocolHeader& header);

  /**
   * Decode message body based on message type
   */
  template<typename T>
  static bool decode_message(const uint8_t* data, size_t size, T& message);
};

}  // namespace network
}  // namespace agentflow
