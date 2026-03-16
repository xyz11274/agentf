// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// C++ Chain Producer Node - Produces initial data to DataBus
// Uses V3 signature with session-isolated DataBus client

#include <string>
#include <chrono>

#include "agent/common.h"
#include "agentflow/so_node.h"

namespace {

using agentflow::DataMap;
using agentflow::DataVariant;
using agentflow::INamespaceClient;
using agentflow::INodeDataBusClient;
using agentflow::SoDataVariant;

std::string get_string(const DataMap& inputs, const std::string& key,
                       const std::string& fallback = "") {
  auto it = inputs.find(key);
  if (it == inputs.end()) {
    return fallback;
  }
  if (const auto* value = std::get_if<std::string>(&it->second)) {
    return *value;
  }
  return fallback;
}

}  // namespace

// V3 signature with session-isolated DataBus client
extern "C" void chain_cpp_producer(const DataMap& inputs, DataMap& outputs,
                                   INodeDataBusClient* client) {
  std::string session_id = get_string(inputs, "session_id", "");
  std::string dialogue_id = get_string(inputs, "dialogue_id", "");

  // Generate initial data with timestamp
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();

  // Write initial data to user namespace
  std::string initial_data = "CPP_DATA_" + std::to_string(timestamp);
  std::string chain_key = dialogue_id.empty() ? "chain_data" : dialogue_id + "/chain_data";

  if (client && client->user()) {
    client->user()->put(chain_key, SoDataVariant(initial_data));
    client->user()->put(chain_key + "_step", SoDataVariant(std::string("cpp")));
    client->user()->put(chain_key + "_timestamp", SoDataVariant(timestamp));
  }

  // Write to sys namespace for framework visibility
  if (client && client->sys()) {
    std::string sys_key = dialogue_id.empty() ? "chain_initiated" : dialogue_id + "/chain_initiated";
    client->sys()->put(sys_key, SoDataVariant(std::string("true")));
  }

  // Output status
  outputs["status"] = DataVariant(std::string("produced"));
  outputs["data_key"] = DataVariant(chain_key);
}