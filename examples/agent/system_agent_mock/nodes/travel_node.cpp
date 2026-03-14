// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Travel Skill Node - Mock travel skill that returns simulated results
// Uses V3 signature with session-isolated DataBus client
// Supports multi-dialogue with dialogue_id isolation

#include <random>
#include <string>

#include "agent/common.h"
#include "agentflow/so_node.h"

namespace {

using agentflow::DataMap;
using agentflow::DataVariant;
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
extern "C" void travel_node_v3(const DataMap& inputs, DataMap& outputs,
                               INodeDataBusClient* client) {
  // Get input parameters
  std::string user_text = get_string(inputs, "user_text", "");
  std::string session_id = get_string(inputs, "session_id", "");
  std::string dialogue_id = get_string(inputs, "dialogue_id", "");

  // Generate mock result
  std::string result =
      "Travel recommendations for '" + user_text + "': "
      "1) Paris, France - romantic city of lights, "
      "2) Tokyo, Japan - blend of tradition and modernity, "
      "3) New York, USA - the city that never sleeps.";

  // Random confidence between 0.7 and 0.95
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<> dis(0.7, 0.95);
  double confidence = dis(gen);

  // Output results
  outputs["skill_result"] = DataVariant(result);
  outputs["status"] = DataVariant(std::string("success"));
  outputs["confidence"] = DataVariant(confidence);

  // Store result in user namespace for coordinator to aggregate
  // With dialogue_id support, the key is: {dialogue_id}/travel_result
  if (client && client->user()) {
    std::string result_key = dialogue_id.empty() ? "travel_result" : dialogue_id + "/travel_result";
    std::string confidence_key = dialogue_id.empty() ? "travel_confidence" : dialogue_id + "/travel_confidence";

    client->user()->put(result_key, SoDataVariant(result));
    client->user()->put(confidence_key, SoDataVariant(confidence));
  }
}