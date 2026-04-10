// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Skill Selection Node - Fixed selection of travel and shopping for E2E testing
// Uses V3 signature with session-isolated DataBus client
// Supports multi-dialogue with dialogue_id isolation
//
// Test scenario: Always selects travel and shopping (fixed, not random)
// Coordinator will add foodie and force-launch shopping

#include <string>
#include <vector>

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

std::string format_json_array(const std::vector<std::string>& items) {
  std::string result = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      result += ",";
    }
    result += "\"" + items[i] + "\"";
  }
  result += "]";
  return result;
}

}  // namespace

// V3 signature with session-isolated DataBus client
extern "C" void skill_selection_node_v3(const DataMap& inputs, DataMap& outputs,
                                        INodeDataBusClient* client) {
  // Get input parameters
  std::string user_text = get_string(inputs, "user_text", "");
  std::string session_id = get_string(inputs, "session_id", "");
  std::string dialogue_id = get_string(inputs, "dialogue_id", "");

  // Fixed selection for E2E test: travel and shopping
  // Coordinator will add foodie and force-launch shopping
  std::vector<std::string> selected = {"travel", "shopping"};
  std::string selected_json = format_json_array(selected);

  // Output next_skills (preferred) and selected_skills (backward compatibility)
  outputs["next_skills"] = DataVariant(selected_json);
  outputs["selected_skills"] = DataVariant(selected_json);
  outputs["selection_reason"] =
      DataVariant("Fixed selection for E2E test: travel and shopping. "
                  "Coordinator will add foodie and force-launch shopping.");

  // Flow trace: output input/output for trace logging
  outputs["af_trace_node_input"] = DataVariant("user_text=" + user_text + "|session=" + session_id + "|dialogue=" + dialogue_id);
  outputs["af_trace_node_output"] = DataVariant("selected_skills=" + selected_json);

  // Write next_skills to sys namespace for framework to launch more skills
  // With dialogue_id support, the key is: {dialogue_id}/next_skills
  if (client && client->sys()) {
    std::string next_skills_key = dialogue_id.empty() ? "next_skills" : dialogue_id + "/next_skills";
    std::string selected_skills_key = dialogue_id.empty() ? "selected_skills" : dialogue_id + "/selected_skills";

    client->sys()->put(next_skills_key, SoDataVariant(selected_json));
    // Also write to selected_skills for backward compatibility
    client->sys()->put(selected_skills_key, SoDataVariant(selected_json));

    // Store user_prompt in dialogue namespace if dialogue_id is provided
    if (!dialogue_id.empty()) {
      client->sys()->put(dialogue_id + "/user_prompt", SoDataVariant(user_text));
    }
  }
}