// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Serialization adapter factory implementation

#include "vendor/serialization/adapter_factory.h"

#include "vendor/serialization/protobuf_impl.h"

namespace agentflow {
namespace serialization {

std::shared_ptr<IProtoAdapter> create_proto_adapter() {
#ifdef AGENTFLOW_WITH_PROTOBUF
  return std::make_shared<ProtoProtobufAdapter>();
#else
  return std::make_shared<ProtoStubAdapter>();
#endif
}

}  // namespace serialization
}  // namespace agentflow
