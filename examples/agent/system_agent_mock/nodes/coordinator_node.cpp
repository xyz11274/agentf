// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Coordinator Node - Aggregates results from skills and decides when to respond
// Uses V3 signature with session-isolated DataBus client
// Supports multi-dialogue with dialogue_id isolation
//
// Design Philosophy:
// - Coordinator checks DataBus for available skill results
// - Makes decisions based on data quality, not pending count
// - skill-selection notification tells coordinator which skills to expect
// - Skills not in selected_skills list are ignored (e.g., skill-selection itself)
//
// E2E Test Scenario:
// 1. skill-selection selects: travel, shopping (stored in selected_skills)
// 2. Coordinator adds foodie to next_skills
// 3. Records results from travel, shopping, foodie, shopping(force)
// 4. Decides when response is ready based on available data

#include <string>
#include <vector>
#include <set>

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

// Parse selected_skills JSON array (simple format: ["skill1", "skill2"])
std::set<std::string> parse_selected_skills(const std::string& json) {
  std::set<std::string> skills;
  if (json.empty()) return skills;

  // Simple JSON array parsing for ["skill1", "skill2"] format
  size_t start = json.find('[');
  size_t end = json.find(']');
  if (start == std::string::npos || end == std::string::npos) return skills;

  std::string content = json.substr(start + 1, end - start - 1);
  size_t pos = 0;
  while ((pos = content.find('"', pos)) != std::string::npos) {
    size_t end_quote = content.find('"', pos + 1);
    if (end_quote == std::string::npos) break;
    skills.insert(content.substr(pos + 1, end_quote - pos - 1));
    pos = end_quote + 1;
  }
  return skills;
}

}  // namespace

// V3 signature with session-isolated DataBus client
extern "C" void coordinator_node_v3(const DataMap& inputs, DataMap& outputs,
                                    INodeDataBusClient* client) {
  // Get input parameters
  std::string session_id = get_string(inputs, "session_id", "");
  std::string user_text = get_string(inputs, "user_text", "");
  std::string dialogue_id = get_string(inputs, "dialogue_id", "");
  std::string completed_skill_name = get_string(inputs, "completed_skill_name", "");

  auto* sys_ns = client ? client->sys() : nullptr;
  auto* user_ns = client ? client->user() : nullptr;

  // State keys with dialogue_id prefix
  std::string foodie_added_key = dialogue_id.empty() ? "foodie_added" : dialogue_id + "/foodie_added";
  std::string shopping_force_launched_key = dialogue_id.empty() ? "shopping_force_launched" : dialogue_id + "/shopping_force_launched";
  std::string selected_skills_key = dialogue_id.empty() ? "selected_skills" : dialogue_id + "/selected_skills";
  std::string next_skills_key = dialogue_id.empty() ? "next_skills" : dialogue_id + "/next_skills";

  // Get selected_skills list (set by skill-selection)
  std::string selected_skills_json = get_optional_string(sys_ns, selected_skills_key);
  std::set<std::string> selected_skills = parse_selected_skills(selected_skills_json);

  // Check if completed_skill is a business skill (in selected_skills) or internal (e.g., skill-selection)
  bool is_business_skill = !completed_skill_name.empty() &&
                           selected_skills.count(completed_skill_name) > 0;

  // ================================================================
  // Step 1: Record skill result if it's a business skill
  // ================================================================
  if (is_business_skill) {
    // Get result from user namespace
    std::string result_key = dialogue_id.empty() ?
        completed_skill_name + "_result" :
        dialogue_id + "/" + completed_skill_name + "_result";
    std::string result = get_optional_string(user_ns, result_key);

    if (!result.empty()) {
      outputs["recorded_skill"] = DataVariant(completed_skill_name);
      outputs["skill_result_preview"] = DataVariant(result.substr(0, 50) + "...");
    }
  } else if (!completed_skill_name.empty()) {
    // skill-selection or other internal skill - just log and continue
    outputs["ignored_skill"] = DataVariant(completed_skill_name);
  }

  // ================================================================
  // Step 2: Add foodie to next_skills (E2E test: dynamic skill addition)
  // ================================================================
  bool foodie_added = get_bool_flag(sys_ns, foodie_added_key);
  if (!foodie_added) {
    std::string next_skills_json = "[{\"name\": \"foodie\"}]";
    outputs["next_skills"] = DataVariant(next_skills_json);
    if (sys_ns) {
      sys_ns->put(next_skills_key, SoDataVariant(next_skills_json));
    }
    set_bool_flag(sys_ns, foodie_added_key, true);

    outputs["status"] = DataVariant(std::string("added_foodie"));
    outputs["action"] = DataVariant(std::string("Added foodie to next_skills"));
    return;
  }

  // ================================================================
  // Step 3: Check shopping result and force-launch if needed
  // ================================================================
  std::string shopping_result_key = dialogue_id.empty() ? "shopping_result" : dialogue_id + "/shopping_result";
  std::string shopping_result = get_optional_string(user_ns, shopping_result_key);
  bool shopping_completed = !shopping_result.empty();

  bool shopping_force_launched = get_bool_flag(sys_ns, shopping_force_launched_key);
  if (shopping_completed && !shopping_force_launched) {
    std::string next_skills_json = "[{\"name\": \"shopping\", \"force\": true}]";
    outputs["next_skills"] = DataVariant(next_skills_json);
    if (sys_ns) {
      sys_ns->put(next_skills_key, SoDataVariant(next_skills_json));
    }
    set_bool_flag(sys_ns, shopping_force_launched_key, true);

    outputs["status"] = DataVariant(std::string("force_launched_shopping"));
    outputs["action"] = DataVariant(std::string("Force-launched shopping after completion"));
    return;
  }

  // ================================================================
  // Step 4: Check available results and decide if response is ready
  // ================================================================
  std::string travel_result_key = dialogue_id.empty() ? "travel_result" : dialogue_id + "/travel_result";
  std::string foodie_result_key = dialogue_id.empty() ? "foodie_result" : dialogue_id + "/foodie_result";

  std::string travel_result = get_optional_string(user_ns, travel_result_key);
  std::string foodie_result = get_optional_string(user_ns, foodie_result_key);

  // Count available results
  int available_results = 0;
  if (!travel_result.empty()) available_results++;
  if (!shopping_result.empty()) available_results++;
  if (!foodie_result.empty()) available_results++;

  // Decision logic: respond when we have at least 2 results, or all 3
  // For E2E test: require all 4 executions (including force-launched shopping)
  bool has_shopping_force = shopping_force_launched;

  // Check if we have enough results to respond
  // E2E test expects: travel + shopping + foodie + shopping(force) = 4 skill executions
  // But results are stored by skill name, so shopping_result is the latest execution
  // For testing: require travel, shopping, foodie all present

  bool response_ready = !travel_result.empty() &&
                        !shopping_result.empty() &&
                        !foodie_result.empty();

  if (!response_ready) {
    outputs["status"] = DataVariant(std::string("waiting_for_results"));
    outputs["available_results"] = DataVariant(available_results);
    std::string missing;
    if (travel_result.empty()) missing += "travel ";
    if (shopping_result.empty()) missing += "shopping ";
    if (foodie_result.empty()) missing += "foodie";
    outputs["missing_skills"] = DataVariant(missing);
    return;
  }

  // ================================================================
  // Step 5: Aggregate results and send response
  // ================================================================
  std::string aggregated = "Aggregated response for query: '" + user_text + "'\n\n";
  aggregated += "[E2E Test Summary]\n";
  aggregated += "- Available results: " + std::to_string(available_results) + "\n";
  aggregated += "- Shopping force-launched: " + std::string(has_shopping_force ? "yes" : "no") + "\n\n";

  int skill_count = 0;
  if (!travel_result.empty()) {
    aggregated += "[Travel Skill]\n" + travel_result + "\n\n";
    ++skill_count;
  }
  if (!shopping_result.empty()) {
    int shopping_exec_count = has_shopping_force ? 2 : 1;
    aggregated += "[Shopping Skill] (executed " + std::to_string(shopping_exec_count) + " times)\n" + shopping_result + "\n\n";
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

  // Flow trace: output input/output for trace logging
  outputs["af_trace_node_input"] = DataVariant("user_text=" + user_text + "|completed_skill=" + completed_skill_name + "|available_results=" + std::to_string(available_results));
  outputs["af_trace_node_output"] = DataVariant("response_ready=true|skills_count=" + std::to_string(skill_count));

  // Write response to sys namespace
  if (sys_ns) {
    std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
    sys_ns->put(response_key, SoDataVariant(aggregated));
  }
}