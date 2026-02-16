// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Protobuf serialization adapter interface

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <map>

namespace agentflow {
namespace serialization {

/**
 * Protobuf error codes
 */
enum class ProtoError {
  PROTO_ERR_OK = 0,
  PROTO_ERR_INIT_FAILED = 1,
  PROTO_ERR_ENCODE_FAILED = 2,
  PROTO_ERR_DECODE_FAILED = 3,
  PROTO_ERR_INVALID_MESSAGE = 4,
  PROTO_ERR_UNKNOWN_FIELD = 5,
};

/**
 * Abstract protobuf message interface
 */
class IProtoMessage {
 public:
  virtual ~IProtoMessage() = default;

  /**
   * Get message type name
   */
  virtual std::string get_type_name() const = 0;

  /**
   * Encode message to bytes
   */
  virtual std::vector<uint8_t> encode() = 0;

  /**
   * Decode message from bytes
   */
  virtual bool decode(const std::vector<uint8_t>& data) = 0;

  /**
   * Clear all fields
   */
  virtual void clear() = 0;

  /**
   * Get message size in bytes
   */
  virtual size_t size() const = 0;

  /**
   * Get debug string representation
   */
  virtual std::string to_string() const = 0;

  /**
   * Merge another message
   */
  virtual bool merge(const IProtoMessage& other) = 0;

  /**
   * Get last error
   */
  virtual ProtoError get_error() const = 0;

  /**
   * Get error message
   */
  virtual std::string get_error_message() const = 0;
};

/**
 * Abstract protobuf message builder
 */
class IProtoMessageBuilder {
 public:
  virtual ~IProtoMessageBuilder() = default;

  /**
   * Create new message instance
   */
  virtual std::shared_ptr<IProtoMessage> create() = 0;

  /**
   * Get message descriptor
   */
  virtual std::string get_descriptor() const = 0;

  /**
   * Get field count
   */
  virtual int get_field_count() const = 0;

  /**
   * Get field name by index
   */
  virtual std::string get_field_name(int index) const = 0;

  /**
   * Get field type by index
   */
  virtual std::string get_field_type(int index) const = 0;
};

/**
 * Abstract protobuf adapter interface
 * Isolates protobuf-c or protobuf from AgentFlow core
 */
class IProtoAdapter {
 public:
  virtual ~IProtoAdapter() = default;

  /**
   * Initialize protobuf library
   */
  virtual bool initialize() = 0;

  /**
   * Load message definition from proto file
   */
  virtual bool load_proto_file(const std::string& file_path) = 0;

  /**
   * Load message definition from descriptor bytes
   */
  virtual bool load_descriptor(const std::vector<uint8_t>& descriptor) = 0;

  /**
   * Get message builder by name
   */
  virtual std::shared_ptr<IProtoMessageBuilder> get_message_builder(
      const std::string& message_name) = 0;

  /**
   * Encode message to JSON string
   */
  virtual std::string message_to_json(const IProtoMessage& message) = 0;

  /**
   * Decode message from JSON string
   */
  virtual bool message_from_json(const std::string& json_str,
                                 IProtoMessage& message) = 0;

  /**
   * Get library version
   */
  virtual std::string get_version() = 0;

  /**
   * Check if protobuf support is available
   */
  virtual bool is_available() const = 0;

  /**
   * Get last error
   */
  virtual ProtoError get_error() const = 0;

  /**
   * Get error message
   */
  virtual std::string get_error_message() const = 0;

  /**
   * Cleanup resources
   */
  virtual void cleanup() = 0;
};

}  // namespace serialization
}  // namespace agentflow
