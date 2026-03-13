// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Skill Selection Node - Randomly selects 2 skills from available skills
// Uses V3 signature with session-isolated DataBus client

#include <algorithm>
#include <random>
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

  // Available skills to choose from
  std::vector<std::string> all_skills = {"travel", "shopping", "foodie"};

  // Randomly shuffle and pick 2 skills
  std::random_device rd;
  std::mt19937 gen(rd());
  std::shuffle(all_skills.begin(), all_skills.end(), gen);

  std::vector<std::string> selected(all_skills.begin(), all_skills.begin() + 2);
  std::string selected_json = format_json_array(selected);

  // Output next_skills (preferred) and selected_skills (backward compatibility)
  outputs["next_skills"] = DataVariant(selected_json);
  outputs["selected_skills"] = DataVariant(selected_json);
  outputs["selection_reason"] =
      DataVariant("Randomly selected " + std::to_string(selected.size()) +
                  " skills for demo based on user query: " + user_text);

  // Write next_skills to sys namespace for framework to launch more skills
  // Use next_skills (preferred) with selected_skills for backward compatibility
  if (client && client->sys()) {
    client->sys()->put("next_skills", SoDataVariant(selected_json));
    // Also write to selected_skills for backward compatibility
    client->sys()->put("selected_skills", SoDataVariant(selected_json));
  }
}