// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Message serializer implementation

#include "agentflow/actor/message_serializer.h"

#include <chrono>
#include <cstring>

#include "agentflow/utils/logger.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR

namespace agentflow {
namespace actor {

// Static member initialization
std::map<std::string, std::function<std::vector<uint8_t>(const std::shared_ptr<void>&)>>
    ProtoactorMessageConverter::custom_serializers_;

std::map<std::string, std::function<bool(const uint8_t*, size_t, std::shared_ptr<void>&, size_t&)>>
    ProtoactorMessageConverter::custom_deserializers_;

std::atomic<uint32_t> ProtoactorMessageConverter::next_message_id_(1);

// ============================================================================
// PayloadSerializer Implementation
// ============================================================================

std::vector<uint8_t> PayloadSerializer::serialize(const DataMap& value) {
  std::vector<uint8_t> buffer;

  // Serialize as map of string -> variant
  // For simplicity, we'll store each value type as a byte code + data

  // Write size
  uint32_t size       = static_cast<uint32_t>(value.size());
  auto     size_bytes = serialize(size);
  buffer.insert(buffer.end(), size_bytes.begin(), size_bytes.end());

  // Write each key-value pair
  for(const auto& [key, val] : value) {
    // Serialize key (string)
    auto     key_bytes     = serialize(key);
    uint32_t key_len       = static_cast<uint32_t>(key_bytes.size());
    auto     key_len_bytes = serialize(key_len);
    buffer.insert(buffer.end(), key_len_bytes.begin(), key_len_bytes.end());
    buffer.insert(buffer.end(), key_bytes.begin(), key_bytes.end());

    // Serialize value type
    uint8_t              type_code = 0;
    std::vector<uint8_t> val_bytes;

    if(std::holds_alternative<int64_t>(val)) {
      type_code = 1;
      val_bytes = serialize(std::get<int64_t>(val));
    } else if(std::holds_alternative<double>(val)) {
      type_code = 2;
      val_bytes = serialize(std::get<double>(val));
    } else if(std::holds_alternative<std::string>(val)) {
      type_code = 3;
      val_bytes = serialize(std::get<std::string>(val));
    } else if(std::holds_alternative<std::vector<uint8_t>>(val)) {
      type_code = 4;
      val_bytes = serialize(std::get<std::vector<uint8_t>>(val));
    } else if(std::holds_alternative<bool>(val)) {
      type_code = 5;
      val_bytes = serialize(std::get<bool>(val));
    }

    buffer.push_back(type_code);
    buffer.insert(buffer.end(), val_bytes.begin(), val_bytes.end());
  }

  return buffer;
}

// ============================================================================
// PayloadDeserializer Implementation
// ============================================================================

bool PayloadDeserializer::deserialize(const uint8_t* data, size_t size, DataMap& output,
                                      size_t& output_bytes_consumed) {
  output_bytes_consumed = 0;

  // Read size
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

  // Read each key-value pair
  for(uint32_t i = 0; i < map_size; ++i) {
    // Read key
    if(size < sizeof(uint32_t)) {
      return false;
    }

    uint32_t key_len;
    if(!deserialize(data, size, key_len, consumed)) {
      return false;
    }
    data += consumed;
    size -= consumed;
    output_bytes_consumed += consumed;

    if(size < key_len) {
      return false;
    }

    std::string key(reinterpret_cast<const char*>(data), key_len);
    data += key_len;
    size -= key_len;
    output_bytes_consumed += key_len;

    // Read value type
    if(size < 1) {
      return false;
    }

    uint8_t type_code = data[0];
    data += 1;
    size -= 1;
    output_bytes_consumed += 1;

    // Deserialize value based on type
    switch(type_code) {
      case 1: {  // int64_t
        int64_t val;
        if(!deserialize(data, size, val, consumed)) {
          return false;
        }
        output[key] = val;
        break;
      }
      case 2: {  // double
        double val;
        if(!deserialize(data, size, val, consumed)) {
          return false;
        }
        output[key] = val;
        break;
      }
      case 3: {  // string
        std::string val;
        if(!deserialize(data, size, val, consumed)) {
          return false;
        }
        output[key] = val;
        break;
      }
      case 4: {  // vector<uint8_t>
        std::vector<uint8_t> val;
        if(!deserialize(data, size, val, consumed)) {
          return false;
        }
        output[key] = val;
        break;
      }
      case 5: {  // bool
        bool val;
        if(!deserialize(data, size, val, consumed)) {
          return false;
        }
        output[key] = val;
        break;
      }
      default:
        return false;
    }

    data += consumed;
    size -= consumed;
    output_bytes_consumed += consumed;
  }

  return true;
}

// ============================================================================
// ProtoactorMessageConverter Implementation
// ============================================================================

std::vector<uint8_t> ProtoactorMessageConverter::actor_message_to_protoactor(
    const ActorMessage& msg, std::shared_ptr<protoactor::PID> target_pid) {
  std::vector<uint8_t> result;

  // Message structure:
  // [4 bytes] message_id
  // [8 bytes] timestamp_ns
  // [4 bytes] message_type_length
  // [N bytes] message_type
  // [1 byte]  has_payload
  // [if has_payload: 1 byte format + N bytes payload]

  // Get message ID
  uint32_t msg_id = next_message_id_.fetch_add(1);

  // Get timestamp
  auto now = std::chrono::high_resolution_clock::now();
  auto timestamp_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

  // Write message ID
  auto msg_id_bytes = PayloadSerializer::serialize(msg_id);
  result.insert(result.end(), msg_id_bytes.begin(), msg_id_bytes.end());

  // Write timestamp
  auto timestamp_bytes = PayloadSerializer::serialize(timestamp_ns);
  result.insert(result.end(), timestamp_bytes.begin(), timestamp_bytes.end());

  // Write message type length and type
  const std::string& msg_type       = msg.message_type;
  uint32_t           type_len       = static_cast<uint32_t>(msg_type.length());
  auto               type_len_bytes = PayloadSerializer::serialize(type_len);
  result.insert(result.end(), type_len_bytes.begin(), type_len_bytes.end());

  result.insert(result.end(), msg_type.begin(), msg_type.end());

  // Write payload indicator
  bool has_payload = (msg.payload != nullptr);
  result.push_back(has_payload ? 1 : 0);

  if(has_payload) {
    // Check for custom serializer for this payload type
    // For now, we'll use RAW format (direct pointer only for local messages)

    // Write format
    result.push_back(static_cast<uint8_t>(SerializationFormat::RAW));

    // For cross-process serialization, we would need to handle this differently
    // For now, we just write a placeholder
    Logger::instance().warn(
        "actor_message_to_protoactor: payload serialization not fully implemented for "
        "cross-process");
  }

  return result;
}

std::shared_ptr<ActorMessage> ProtoactorMessageConverter::protoactor_to_actor_message(
    const uint8_t* data, size_t size) {
  if(!data || size < sizeof(uint32_t)) {
    return nullptr;
  }

  auto   result = std::make_shared<ActorMessage>();
  size_t offset = 0;

  // Read message ID
  uint32_t msg_id;
  size_t   consumed;
  if(!PayloadDeserializer::deserialize(data + offset, size - offset, msg_id, consumed)) {
    return nullptr;
  }
  offset += consumed;

  // Read timestamp
  uint64_t timestamp_ns;
  if(!PayloadDeserializer::deserialize(data + offset, size - offset, timestamp_ns, consumed)) {
    return nullptr;
  }
  offset += consumed;

  // Read message type length
  uint32_t type_len;
  if(!PayloadDeserializer::deserialize(data + offset, size - offset, type_len, consumed)) {
    return nullptr;
  }
  offset += consumed;

  // Read message type
  if(offset + type_len > size) {
    return nullptr;
  }

  result->message_type = std::string(reinterpret_cast<const char*>(data + offset), type_len);
  offset += type_len;

  // Read payload indicator
  if(offset >= size) {
    return nullptr;
  }

  bool has_payload = (data[offset] != 0);
  offset += 1;

  if(has_payload && offset < size) {
    // Read format
    uint8_t format = data[offset];
    offset += 1;

    SerializationFormat fmt = static_cast<SerializationFormat>(format);

    if(fmt == SerializationFormat::RAW) {
      // RAW format - payload is not serialized, used only for local messages
      // In this case, we can't deserialize from bytes
      Logger::instance().warn(
          "protoactor_to_actor_message: RAW format payload cannot be deserialized from bytes");
      result->payload = nullptr;
    } else {
      // Other formats would be implemented here
      Logger::instance().warn("protoactor_to_actor_message: unsupported serialization format");
      result->payload = nullptr;
    }
  }

  return result;
}

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
