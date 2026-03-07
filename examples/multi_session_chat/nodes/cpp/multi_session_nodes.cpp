// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <string>

#include "agent/common.h"

namespace {

using agentflow::DataMap;
using agentflow::DataVariant;

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

bool get_bool(const DataMap& inputs, const std::string& key, bool fallback = false) {
  auto it = inputs.find(key);
  if (it == inputs.end()) {
    return fallback;
  }
  if (const auto* value = std::get_if<bool>(&it->second)) {
    return *value;
  }
  return fallback;
}

}  // namespace

// These compatibility exports are kept so the example shared library still builds
// cleanly, even though the current multi_session_chat package now routes
// dispatcher/coordinator logic through shell workflows and DataBus.

extern "C" void node_pipeline(const DataMap& inputs, DataMap& outputs) {
  outputs["selected_skills"] = get_string(inputs, "selected_skills", "[]");
  outputs["dispatcher_done"] = get_bool(inputs, "dispatcher_done", true);
}

extern "C" void node_collect_skills(const DataMap& inputs, DataMap& outputs) {
  outputs["selected_skills"] = get_string(inputs, "selected_skills", "[]");
}

extern "C" void node_plan_dependencies(const DataMap& inputs, DataMap& outputs) {
  outputs["task_plan"] = get_string(inputs, "task_plan", "[]");
}

extern "C" void node_apply_task_plan(const DataMap& inputs, DataMap& outputs) {
  outputs["orchestration_plan"] = get_string(inputs, "orchestration_plan", "[]");
  outputs["final_response"] = get_string(inputs, "final_response", "");
}

extern "C" void node_load_context(const DataMap& inputs, DataMap& outputs) {
  outputs["session_context"] = get_string(inputs, "session_context", "{}");
}

extern "C" void node_select_skill(const DataMap& inputs, DataMap& outputs) {
  outputs["candidate_list"] = get_string(inputs, "candidate_list", "[]");
}

extern "C" void node_create_task(const DataMap& inputs, DataMap& outputs) {
  outputs["selected_skills"] = get_string(inputs, "selected_skills", "[]");
}
