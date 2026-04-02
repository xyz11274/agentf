// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Coordinator Node - Aggregates results from all skills and produces final response
// Uses V3 signature with session-isolated DataBus client
// Supports multi-dialogue with dialogue_id isolation
//
// E2E Test Scenario:
// 1. Selection workflow selects: travel, shopping (fixed)
// 2. Coordinator adds foodie to next_skills
// 3. When shopping completes, coordinator force-launches shopping again
// 4. Final response aggregates all skill results

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

long get_pending_skills_count(INamespaceClient* ns, const std::string& dialogue_id) {
  if (!ns) {
    return 0;
  }
  std::string pending_key = dialogue_id.empty() ? "pending_skills_count" : dialogue_id + "/pending_skills_count";
  SoDataVariant value;
  if (ns->get(pending_key, value)) {
    if (const auto* count = std::get_if<long>(&value)) {
      return *count;
    } else if (const auto* count_str = std::get_if<std::string>(&value)) {
      return std::stol(*count_str);
    }
  }
  return 0;
}

bool get_bool_flag(INamespaceClient* ns, const std::string& key) {
  if (!ns) {
    return false;
  }
  SoDataVariant value;
  if (ns->get(key, value)) {
    if (const auto* text = std::get_if<std::string>(&value)) {
      return (*text == "true");
    }
  }
  return false;
}

void set_bool_flag(INamespaceClient* ns, const std::string& key, bool value) {
  if (ns) {
    ns->put(key, SoDataVariant(value ? "true" : "false"));
  }
}

}  // namespace

// V3 signature with session-isolated DataBus client
extern "C" void coordinator_node_v3(const DataMap& inputs, DataMap& outputs,
                                    INodeDataBusClient* client) {
  // Get input parameters
  std::string session_id = get_string(inputs, "session_id", "");
  std::string user_text = get_string(inputs, "user_text", "");
  std::string dialogue_id = get_string(inputs, "dialogue_id", "");

  auto* sys_ns = client ? client->sys() : nullptr;
  auto* user_ns = client ? client->user() : nullptr;

  // State keys with dialogue_id prefix
  std::string foodie_added_key = dialogue_id.empty() ? "foodie_added" : dialogue_id + "/foodie_added";
  std::string shopping_force_launched_key = dialogue_id.empty() ? "shopping_force_launched" : dialogue_id + "/shopping_force_launched";
  std::string shopping_result_key = dialogue_id.empty() ? "shopping_result" : dialogue_id + "/shopping_result";

  // Check state flags
  bool foodie_added = get_bool_flag(sys_ns, foodie_added_key);
  bool shopping_force_launched = get_bool_flag(sys_ns, shopping_force_launched_key);

  // Get shopping result to check if shopping has completed
  std::string shopping_result = get_optional_string(user_ns, shopping_result_key);
  bool shopping_completed = !shopping_result.empty();

  // ================================================================
  // Step 1: Add foodie to next_skills if not already added
  // ================================================================
  if (!foodie_added) {
    // Output next_skills with foodie added
    // Format: [{"name": "foodie"}] - simple skill addition
    std::string next_skills_json = "[{\"name\": \"foodie\"}]";
    outputs["next_skills"] = DataVariant(next_skills_json);

    // Write to sys namespace for framework to launch
    std::string next_skills_key = dialogue_id.empty() ? "next_skills" : dialogue_id + "/next_skills";
    sys_ns->put(next_skills_key, SoDataVariant(next_skills_json));

    // Set flag to prevent re-adding
    set_bool_flag(sys_ns, foodie_added_key, true);

    outputs["status"] = DataVariant(std::string("added_foodie"));
    outputs["action"] = DataVariant(std::string("Added foodie to next_skills"));
    return;
  }

  // ================================================================
  // Step 2: Force-launch shopping if it completed and not yet force-launched
  // ================================================================
  if (shopping_completed && !shopping_force_launched) {
    // Output next_skills with force=true for shopping
    // Format: [{"name": "shopping", "force": true}]
    std::string next_skills_json = "[{\"name\": \"shopping\", \"force\": true}]";
    outputs["next_skills"] = DataVariant(next_skills_json);

    // Write to sys namespace for framework to launch
    std::string next_skills_key = dialogue_id.empty() ? "next_skills" : dialogue_id + "/next_skills";
    sys_ns->put(next_skills_key, SoDataVariant(next_skills_json));

    // Set flag to prevent re-launching
    set_bool_flag(sys_ns, shopping_force_launched_key, true);

    outputs["status"] = DataVariant(std::string("force_launched_shopping"));
    outputs["action"] = DataVariant(std::string("Force-launched shopping after completion"));
    return;
  }

  // ================================================================
  // Step 3: Check if all skills have completed
  // ================================================================
  long pending_count = get_pending_skills_count(sys_ns, dialogue_id);
  if (pending_count > 0) {
    // Not all skills have completed yet - don't send response
    // The workflow will be triggered again when the next skill completes
    outputs["status"] = DataVariant(std::string("waiting_for_skills"));
    outputs["pending_skills"] = DataVariant(pending_count);
    return;
  }

  // ================================================================
  // Step 4: All skills completed - aggregate results
  // ================================================================
  std::string travel_key = dialogue_id.empty() ? "travel_result" : dialogue_id + "/travel_result";
  std::string foodie_key = dialogue_id.empty() ? "foodie_result" : dialogue_id + "/foodie_result";

  std::string travel_result = get_optional_string(user_ns, travel_key);
  std::string foodie_result = get_optional_string(user_ns, foodie_key);

  // Count how many times shopping was executed (should be 2: original + force)
  int shopping_count = shopping_force_launched ? 2 : 1;

  // Aggregate results
  std::string aggregated = "Aggregated response for query: '" + user_text + "'\n\n";

  int skill_count = 0;
  if (!travel_result.empty()) {
    aggregated += "[Travel Skill]\n" + travel_result + "\n\n";
    ++skill_count;
  }
  if (!shopping_result.empty()) {
    aggregated += "[Shopping Skill] (executed " + std::to_string(shopping_count) + " times via force)\n" + shopping_result + "\n\n";
    ++skill_count;
  }
  if (!foodie_result.empty()) {
    aggregated += "[Foodie Skill]\n" + foodie_result + "\n\n";
    ++skill_count;
  }

  aggregated += "Total " + std::to_string(skill_count) + " skills contributed to this response.";
  aggregated += "\n\nE2E Test: foodie_added=" + std::string(foodie_added ? "true" : "false");
  aggregated += ", shopping_force_launched=" + std::string(shopping_force_launched ? "true" : "false");

  // Output results
  outputs["response"] = DataVariant(aggregated);
  outputs["final_answer"] = DataVariant(aggregated);
  outputs["coordinator_done"] = DataVariant(std::string("true"));

  // Write response to sys namespace for framework to return to user
  if (sys_ns) {
    std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
    sys_ns->put(response_key, SoDataVariant(aggregated));
  }
}