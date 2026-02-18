// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Serialization adapter factory

#pragma once

#include <memory>

#include "serialization/proto_adapter.h"

namespace agentflow {
namespace serialization {

/**
 * Create protobuf adapter instance
 * Uses protobuf if available, otherwise returns stub
 */
std::shared_ptr<IProtoAdapter> create_proto_adapter();

}  // namespace serialization
}  // namespace agentflow
