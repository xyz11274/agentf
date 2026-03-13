// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Coordinator Node - Aggregates results from all skills and produces final response
// Uses V3 signature with session-isolated DataBus client

#include <string>

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

std::string get_optional_string(INamespaceClient* ns, const std::string& key) {
  if (!ns) {
    return "";
  }
  SoDataVariant value;
  if (ns->get(key, value)) {
    if (const auto* text = std::get_if<std::string>(&value)) {
      return *text;
    }
  }
  return "";
}

}  // namespace

// V3 signature with session-isolated DataBus client
extern "C" void coordinator_node_v3(const DataMap& inputs, DataMap& outputs,
                                    INodeDataBusClient* client) {
  // Get input parameters
  std::string session_id = get_string(inputs, "session_id", "");
  std::string user_text = get_string(inputs, "user_text", "");

  // Collect results from all skills via DataBus user namespace
  std::string travel_result = get_optional_string(client ? client->user() : nullptr, "travel_result");
  std::string shopping_result = get_optional_string(client ? client->user() : nullptr, "shopping_result");
  std::string foodie_result = get_optional_string(client ? client->user() : nullptr, "foodie_result");

  // Aggregate results
  std::string aggregated = "Aggregated response for query: '" + user_text + "'\n\n";

  int skill_count = 0;
  if (!travel_result.empty()) {
    aggregated += "[Travel Skill]\n" + travel_result + "\n\n";
    ++skill_count;
  }
  if (!shopping_result.empty()) {
    aggregated += "[Shopping Skill]\n" + shopping_result + "\n\n";
    ++skill_count;
  }
  if (!foodie_result.empty()) {
    aggregated += "[Foodie Skill]\n" + foodie_result + "\n\n";
    ++skill_count;
  }

  aggregated += "Total " + std::to_string(skill_count) + " skills contributed to this response.";

  // Output results
  outputs["response"] = DataVariant(aggregated);
  outputs["final_answer"] = DataVariant(aggregated);
  outputs["coordinator_done"] = DataVariant(std::string("true"));

  // Write response to sys namespace for framework to return to user
  if (client && client->sys()) {
    client->sys()->put("response", SoDataVariant(aggregated));
  }
}