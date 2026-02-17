// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Protobuf-c implementation for serialization

#pragma once

#include "agentflow/serialization/proto_adapter.h"

#ifdef AGENTFLOW_WITH_PROTOBUF

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#endif

namespace agentflow {
namespace serialization {

#ifdef AGENTFLOW_WITH_PROTOBUF

/**
 * Protobuf message wrapper implementation
 */
class ProtoProtobufMessage : public IProtoMessage {
public:
  explicit ProtoProtobufMessage(std::shared_ptr<google::protobuf::Message> message)
      : message_(message) {}

  ~ProtoProtobufMessage() override = default;

  std::string          get_type_name() const override;
  std::vector<uint8_t> encode() override;
  bool                 decode(const std::vector<uint8_t>& data) override;
  void                 clear() override;
  size_t               size() const override;
  std::string          to_string() const override;
  bool                 merge(const IProtoMessage& other) override;
  ProtoError           get_error() const override { return error_; }
  std::string          get_error_message() const override { return error_msg_; }

private:
  std::shared_ptr<google::protobuf::Message> message_;
  mutable ProtoError                         error_;
  mutable std::string                        error_msg_;
};

/**
 * Protobuf message builder implementation
 */
class ProtoProtobufMessageBuilder : public IProtoMessageBuilder {
public:
  explicit ProtoProtobufMessageBuilder(const google::protobuf::Descriptor* descriptor)
      : descriptor_(descriptor) {}

  ~ProtoProtobufMessageBuilder() override = default;

  std::shared_ptr<IProtoMessage> create() override;
  std::string                    get_descriptor() const override;
  int                            get_field_count() const override;
  std::string                    get_field_name(int index) const override;
  std::string                    get_field_type(int index) const override;

private:
  const google::protobuf::Descriptor* descriptor_;
};

/**
 * Protobuf adapter implementation
 */
class ProtoProtobufAdapter : public IProtoAdapter {
public:
  ProtoProtobufAdapter();
  ~ProtoProtobufAdapter() override;

  bool initialize() override;
  bool load_proto_file(const std::string& file_path) override;
  bool load_descriptor(const std::vector<uint8_t>& descriptor) override;
  std::shared_ptr<IProtoMessageBuilder> get_message_builder(
      const std::string& message_name) override;
  std::string message_to_json(const IProtoMessage& message) override;
  bool        message_from_json(const std::string& json_str, IProtoMessage& message) override;
  std::string get_version() override;
  bool        is_available() const override { return available_; }
  ProtoError  get_error() const override { return error_; }
  std::string get_error_message() const override { return error_msg_; }
  void        cleanup() override;

private:
  bool                available_;
  mutable ProtoError  error_;
  mutable std::string error_msg_;
};

#else

/**
 * Stub protobuf message for when protobuf is not available
 */
class ProtoStubMessage : public IProtoMessage {
public:
  std::string          get_type_name() const override { return ""; }
  std::vector<uint8_t> encode() override { return {}; }
  bool                 decode(const std::vector<uint8_t>&) override { return false; }
  void                 clear() override {}
  size_t               size() const override { return 0; }
  std::string          to_string() const override { return ""; }
  bool                 merge(const IProtoMessage&) override { return false; }
  ProtoError           get_error() const override { return ProtoError::PROTO_ERR_INIT_FAILED; }
  std::string          get_error_message() const override { return "Protobuf not available"; }
};

/**
 * Stub protobuf message builder
 */
class ProtoStubMessageBuilder : public IProtoMessageBuilder {
public:
  std::shared_ptr<IProtoMessage> create() override { return std::make_shared<ProtoStubMessage>(); }
  std::string                    get_descriptor() const override { return ""; }
  int                            get_field_count() const override { return 0; }
  std::string                    get_field_name(int) const override { return ""; }
  std::string                    get_field_type(int) const override { return ""; }
};

/**
 * Stub protobuf adapter
 */
class ProtoStubAdapter : public IProtoAdapter {
public:
  bool initialize() override { return false; }
  bool load_proto_file(const std::string&) override { return false; }
  bool load_descriptor(const std::vector<uint8_t>&) override { return false; }
  std::shared_ptr<IProtoMessageBuilder> get_message_builder(const std::string&) override {
    return std::make_shared<ProtoStubMessageBuilder>();
  }
  std::string message_to_json(const IProtoMessage&) override { return ""; }
  bool        message_from_json(const std::string&, IProtoMessage&) override { return false; }
  std::string get_version() override { return "stub"; }
  bool        is_available() const override { return false; }
  ProtoError  get_error() const override { return ProtoError::PROTO_ERR_INIT_FAILED; }
  std::string get_error_message() const override { return "Protobuf not available"; }
  void        cleanup() override {}
};

#endif  // AGENTFLOW_WITH_PROTOBUF

}  // namespace serialization
}  // namespace agentflow
