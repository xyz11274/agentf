// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Message serializer for converting between ActorMessage and Protoactor messages

#ifndef AGENTFLOW_ACTOR_MESSAGE_SERIALIZER_H
#define AGENTFLOW_ACTOR_MESSAGE_SERIALIZER_H

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vendor/actor/adapter.h"
#include "agent/common.h"

// Forward declare Protoactor types
namespace protoactor {
class PID;
}

namespace agentflow {
namespace actor {

/// @brief Serialized message format
enum class SerializationFormat : uint8_t {
  RAW      = 0,  // Raw pointer - no serialization (local only)
  JSON     = 1,  // JSON format
  PROTOBUF = 2,  // Protocol Buffers
  BINARY   = 3,  // Custom binary format
};

/// @brief Message metadata
struct MessageMetadata {
  std::string         message_type;
  SerializationFormat format;
  std::string         sender_address;
  std::string         receiver_address;
  uint64_t            timestamp_ns;
  uint32_t            message_id;

  MessageMetadata() : format(SerializationFormat::RAW), timestamp_ns(0), message_id(0) {}
};

/// @brief Serializes various payload types
class PayloadSerializer {
public:
  /// @brief Serialize a value to bytes
  /// @tparam T Type to serialize
  /// @param value Value to serialize
  /// @return Serialized bytes
  template <typename T>
  static std::vector<uint8_t> serialize(const T& value) {
    std::vector<uint8_t> buffer(sizeof(T));
    std::memcpy(buffer.data(), &value, sizeof(T));
    return buffer;
  }

  /// @brief Serialize a string
  static std::vector<uint8_t> serialize(const std::string& value) {
    std::vector<uint8_t> buffer(value.begin(), value.end());
    return buffer;
  }

  /// @brief Serialize a vector
  template <typename T>
  static std::vector<uint8_t> serialize(const std::vector<T>& value) {
    std::vector<uint8_t> buffer;
    // First write size
    uint32_t size       = static_cast<uint32_t>(value.size());
    auto     size_bytes = serialize(size);
    buffer.insert(buffer.end(), size_bytes.begin(), size_bytes.end());
    // Then write each element
    for(const auto& elem : value) {
      auto elem_bytes = serialize(elem);
      buffer.insert(buffer.end(), elem_bytes.begin(), elem_bytes.end());
    }
    return buffer;
  }

  /// @brief Serialize a map
  template <typename K, typename V>
  static std::vector<uint8_t> serialize(const std::map<K, V>& value) {
    std::vector<uint8_t> buffer;
    // First write size
    uint32_t size       = static_cast<uint32_t>(value.size());
    auto     size_bytes = serialize(size);
    buffer.insert(buffer.end(), size_bytes.begin(), size_bytes.end());
    // Then write each key-value pair
    for(const auto& [key, val] : value) {
      auto key_bytes = serialize(key);
      auto val_bytes = serialize(val);
      buffer.insert(buffer.end(), key_bytes.begin(), key_bytes.end());
      buffer.insert(buffer.end(), val_bytes.begin(), val_bytes.end());
    }
    return buffer;
  }

  /// @brief Serialize DataMap
  static std::vector<uint8_t> serialize(const DataMap& value);

  /// @brief Serialize shared_ptr to any type
  template <typename T>
  static std::vector<uint8_t> serialize(const std::shared_ptr<T>& value) {
    if(!value) {
      return std::vector<uint8_t>{ 0 };  // Null indicator
    }
    return serialize(*value);
  }
};

/// @brief Deserializes bytes back to values
class PayloadDeserializer {
public:
  /// @brief Deserialize bytes to a value
  /// @tparam T Type to deserialize to
  /// @param data Input bytes
  /// @param size Size of input bytes
  /// @param output_bytes_consumed Bytes consumed (output)
  /// @return Deserialized value
  template <typename T>
  static bool deserialize(const uint8_t* data, size_t size, T& output,
                          size_t& output_bytes_consumed) {
    if(size < sizeof(T)) {
      return false;
    }
    std::memcpy(&output, data, sizeof(T));
    output_bytes_consumed = sizeof(T);
    return true;
  }

  /// @brief Deserialize bytes to a string
  static bool deserialize(const uint8_t* data, size_t size, std::string& output,
                          size_t& output_bytes_consumed) {
    output                = std::string(reinterpret_cast<const char*>(data), size);
    output_bytes_consumed = size;
    return true;
  }

  /// @brief Deserialize bytes to a vector
  template <typename T>
  static bool deserialize(const uint8_t* data, size_t size, std::vector<T>& output,
                          size_t& output_bytes_consumed) {
    output_bytes_consumed = 0;
    if(size < sizeof(uint32_t)) {
      return false;
    }
    uint32_t vec_size;
    size_t   consumed;
    if(!deserialize(data, size, vec_size, consumed)) {
      return false;
    }
    data += consumed;
    size -= consumed;
    output_bytes_consumed += consumed;

    output.reserve(vec_size);
    for(uint32_t i = 0; i < vec_size; ++i) {
      T elem;
      if(!deserialize(data, size, elem, consumed)) {
        return false;
      }
      output.push_back(elem);
      data += consumed;
      size -= consumed;
      output_bytes_consumed += consumed;
    }
    return true;
  }

  /// @brief Deserialize bytes to a map
  template <typename K, typename V>
  static bool deserialize(const uint8_t* data, size_t size, std::map<K, V>& output,
                          size_t& output_bytes_consumed) {
    output_bytes_consumed = 0;
    if(size < sizeof(uint32_t)) {
      return false;
    }
    uint32_t map_size;
    size_t   consumed;
    if(!deserialize(data, size, map_size, consumed)) {
      return false;
    }
    data += consumed;
    size -= consumed;
    output_bytes_consumed += consumed;

    for(uint32_t i = 0; i < map_size; ++i) {
      K key;
      V val;
      if(!deserialize(data, size, key, consumed)) {
        return false;
      }
      data += consumed;
      size -= consumed;
      output_bytes_consumed += consumed;
      if(!deserialize(data, size, val, consumed)) {
        return false;
      }
      data += consumed;
      size -= consumed;
      output_bytes_consumed += consumed;
      output[key] = val;
    }
    return true;
  }

  /// @brief Deserialize bytes to DataMap
  static bool deserialize(const uint8_t* data, size_t size, DataMap& output,
                          size_t& output_bytes_consumed);

  /// @brief Deserialize bytes to shared_ptr
  template <typename T>
  static bool deserialize(const uint8_t* data, size_t size, std::shared_ptr<T>& output,
                          size_t& output_bytes_consumed) {
    output_bytes_consumed = 0;
    // Check for null indicator
    if(size >= 1 && data[0] == 0) {
      output                = nullptr;
      output_bytes_consumed = 1;
      return true;
    }
    auto result = std::make_shared<T>();
    if(!deserialize(data, size, *result, output_bytes_consumed)) {
      return false;
    }
    output = result;
    return true;
  }
};

/// @brief Converts ActorMessage to Protoactor message
class ProtoactorMessageConverter {
public:
  /// @brief Convert ActorMessage to Protoactor message
  /// @param msg The ActorMessage to convert
  /// @param target_pid The target PID (if applicable)
  /// @return Serialized message bytes
  static std::vector<uint8_t> actor_message_to_protoactor(
      const ActorMessage& msg, std::shared_ptr<protoactor::PID> target_pid = nullptr);

  /// @brief Convert Protoactor message bytes to ActorMessage
  /// @param data The message bytes
  /// @param size Size of message bytes
  /// @return ActorMessage or nullptr on error
  static std::shared_ptr<ActorMessage> protoactor_to_actor_message(const uint8_t* data,
                                                                   size_t         size);

  /// @brief Register a custom message serializer for a specific type
  /// @tparam T Message type
  /// @param serializer Function to serialize
  /// @param deserializer Function to deserialize
  template <typename T>
  static void register_serializer(
      std::function<std::vector<uint8_t>(const T&)>            serializer,
      std::function<bool(const uint8_t*, size_t, T&, size_t&)> deserializer) {
    custom_serializers_[typeid(T).name()] = [serializer](const std::shared_ptr<void>& payload) {
      const T* ptr = static_cast<const T*>(payload.get());
      return serializer(*ptr);
    };
    custom_deserializers_[typeid(T).name()] = [deserializer](const uint8_t* data, size_t size,
                                                             std::shared_ptr<void>& output,
                                                             size_t&                consumed) {
      auto result = std::make_shared<T>();
      if(deserializer(data, size, *result, consumed)) {
        output = result;
        return true;
      }
      return false;
    };
  }

private:
  static std::map<std::string, std::function<std::vector<uint8_t>(const std::shared_ptr<void>&)>>
      custom_serializers_;
  static std::map<std::string,
                  std::function<bool(const uint8_t*, size_t, std::shared_ptr<void>&, size_t&)>>
                               custom_deserializers_;
  static std::atomic<uint32_t> next_message_id_;
};

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR

#endif  // AGENTFLOW_ACTOR_MESSAGE_SERIALIZER_H
