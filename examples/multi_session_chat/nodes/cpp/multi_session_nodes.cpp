// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// multi_session_nodes.cpp
// C++ SO library implementing nodes for:
//   - default-dispatcher-workflow  (node: pipeline)
//   - default-coordinator-workflow (nodes: collect, plan, apply)
//   - session-dispatcher-workflow  (nodes: load_context, select_skill, create_task)
//
// Each exported function follows the SoFunction signature:
//   void func(const DataMap& inputs, DataMap& outputs)
//
// Inputs arrive from the agent's shared DataBus via workflow edge propagation.
// Outputs are written back to the DataBus by the framework after execution.
//
// DataBus key convention used across workflows:
//   session:<session_id>:skill_selected   — skill name chosen for a session
//   session:<session_id>:skill_result     — result text from skill execution
//   session:<session_id>:context          — session context JSON
//   coordinator:selected_skills           — JSONL of all sessions' skill selections
//   coordinator:task_plan                 — dependency plan from LLM
//   coordinator:orchestration_plan        — start/stop/skip decisions per task

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "agent/common.h"

namespace {

using agentflow::DataMap;
using agentflow::DataVariant;

// ---- Helpers ----------------------------------------------------------------

std::string get_str(const DataMap& m, const std::string& key,
                    const std::string& def = "") {
  auto it = m.find(key);
  if(it == m.end()) return def;
  if(auto* s = std::get_if<std::string>(&it->second)) return *s;
  return def;
}

// Escape a string for JSON value embedding (not key safe, basic escaping).
std::string json_esc(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for(char c : s) {
    switch(c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  if(!f.is_open()) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::trunc);
  if(!f.is_open()) return false;
  f << content;
  return true;
}

// Run a shell command and capture its stdout output.
std::string run_cmd(const std::string& cmd) {
  std::array<char, 256> buf{};
  std::string result;
  // NOLINTNEXTLINE(cert-env33-c)
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
  if(!pipe) return "";
  while(fgets(buf.data(), buf.size(), pipe.get()) != nullptr) {
    result += buf.data();
  }
  // Trim trailing newline
  while(!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

// Resolve the package root from the AF_PACKAGE_ROOT env var or a relative path.
// Nodes receive this as an input key "package_root" (injected by the runtime or workflow).
std::string resolve_root(const DataMap& inputs) {
  std::string root = get_str(inputs, "package_root");
  if(root.empty()) {
    const char* env = std::getenv("AF_PACKAGE_ROOT");
    if(env) root = env;
  }
  if(root.empty()) {
    // Last resort: look for the scripts directory relative to cwd
    root = "examples/multi_session_chat/package";
  }
  return root;
}

static const std::vector<std::string> kSessionIds = {
    "precreated_session_1", "precreated_session_2",
    "precreated_session_3", "precreated_session_4"};

static const std::vector<std::string> kSkills = {
    "skill_search", "skill_summary", "skill_date", "skill_delegate"};

// Simple JSON field extraction (value of first occurrence of "key":"...")
std::string json_get_str(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  auto pos = json.find(needle);
  if(pos == std::string::npos) return "";
  pos = json.find(':', pos + needle.size());
  if(pos == std::string::npos) return "";
  ++pos;
  while(pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if(pos >= json.size() || json[pos] != '"') return "";
  ++pos;
  std::string val;
  while(pos < json.size() && json[pos] != '"') {
    if(json[pos] == '\\' && pos + 1 < json.size()) { ++pos; val += json[pos]; }
    else val += json[pos];
    ++pos;
  }
  return val;
}

}  // namespace

// ============================================================================
// default-dispatcher-workflow: node "pipeline"
// ============================================================================
// Orchestrates the full per-message pipeline in the default session:
//   broadcast → per-session skill selection → collect → execute → compose reply
// Inputs:  user_text, session_id, package_root (all optional; fall back to env/files)
// Outputs: final_response
extern "C" void node_pipeline(const DataMap& inputs, DataMap& outputs) {
  std::string root      = resolve_root(inputs);
  std::string user_text = get_str(inputs, "user_text");
  std::string session_id = get_str(inputs, "session_id", "default");
  std::string scripts   = root + "/scripts";
  std::string runtime   = root + "/runtime";

  std::filesystem::create_directories(runtime);

  // 1. Broadcast user input to all session inboxes
  if(!user_text.empty()) {
    write_file(runtime + "/latest_user_input.txt", user_text);
    for(const auto& sid : kSessionIds) {
      std::ofstream f(runtime + "/" + sid + ".inbox", std::ios::app);
      f << "user=" << user_text << "\n";
    }
  }

  // 2. Per-session: select skill (calls LLM or uses fallback) + create task record
  std::string latest_user =
      read_file(runtime + "/latest_user_input.txt");
  if(latest_user.empty()) latest_user = user_text;

  const std::vector<std::string> fallback_skills = {
      "skill_search", "skill_summary", "skill_date", "skill_delegate"};
  for(size_t i = 0; i < kSessionIds.size(); ++i) {
    const std::string& sid = kSessionIds[i];
    // Try LLM-based selection; fall back to round-robin assignment
    std::string skill_cmd =
        "AF_SESSION_ID='" + sid + "' AF_INPUT='user=" + json_esc(latest_user) +
        "' '" + scripts + "/select_skill.sh' 2>/dev/null";
    run_cmd(skill_cmd);

    // Ensure skill file exists (select_skill.sh writes it)
    std::string skill_file = runtime + "/" + sid + ".skill.json";
    if(!std::filesystem::exists(skill_file)) {
      std::string fallback = fallback_skills[i % fallback_skills.size()];
      std::string json = "{\"session_id\":\"" + sid + "\",\"skill\":\"" + fallback +
                         "\",\"params\":{\"query\":\"" + json_esc(latest_user) + "\"}}";
      write_file(skill_file, json);
    }

    // Create task record (not-started state)
    std::string task_id  = sid + "_skill_task";
    std::string skill_content = read_file(skill_file);
    std::string task_json = "{\"task_id\":\"" + task_id +
                            "\",\"status\":\"created_not_started\",\"skill\":" +
                            skill_content + "}";
    write_file(runtime + "/" + task_id + ".json", task_json);
  }

  // 3. Execute each session's selected skill
  std::string skill_results_path = runtime + "/skill_results.jsonl";
  write_file(skill_results_path, "");  // clear
  for(const auto& sid : kSessionIds) {
    std::string skill_file = runtime + "/" + sid + ".skill.json";
    if(!std::filesystem::exists(skill_file)) continue;
    std::string skill_json = read_file(skill_file);
    std::string skill      = json_get_str(skill_json, "skill");
    if(skill.empty()) skill = "skill_search";

    std::string skill_script = scripts + "/" + skill + ".sh";
    if(!std::filesystem::exists(skill_script)) {
      skill_script = scripts + "/skill_search.sh";
    }
    std::string result = run_cmd("AF_USER_TEXT='" + json_esc(latest_user) +
                                 "' AF_SESSION_ID='" + sid + "' '" + skill_script +
                                 "' 2>/dev/null");

    std::ofstream rf(skill_results_path, std::ios::app);
    rf << "{\"session_id\":\"" << sid << "\",\"skill\":\"" << skill
       << "\",\"result\":\"" << json_esc(result) << "\"}\n";
  }

  // 4. Compose final response via LLM or local fallback
  std::string compose_cmd =
      "'" + scripts + "/compose_final_response.sh' 2>/dev/null";
  std::string final_response = run_cmd(compose_cmd);
  if(final_response.empty()) {
    final_response = read_file(runtime + "/final_response.txt");
  }
  if(final_response.empty()) {
    final_response = "已完成多技能分析，请查阅详细结果。";
  }

  outputs["final_response"] = final_response;
  outputs["skill_results"]  = read_file(skill_results_path);
}

// ============================================================================
// default-coordinator-workflow: node "collect"
// ============================================================================
// Collects skill selection results from all session inboxes into a unified JSONL.
// Inputs:  package_root
// Outputs: selected_skills (JSONL text), selected_skills_count
extern "C" void node_collect_skills(const DataMap& inputs, DataMap& outputs) {
  std::string root    = resolve_root(inputs);
  std::string runtime = root + "/runtime";
  std::filesystem::create_directories(runtime);

  std::string out_path = runtime + "/selected_skills.jsonl";
  std::ofstream out(out_path, std::ios::trunc);
  int count = 0;
  for(const auto& sid : kSessionIds) {
    std::string fpath = runtime + "/" + sid + ".skill.json";
    if(!std::filesystem::exists(fpath)) continue;
    std::string content = read_file(fpath);
    if(!content.empty()) {
      out << content << "\n";
      ++count;
    }
  }
  out.close();

  outputs["selected_skills"]       = read_file(out_path);
  outputs["selected_skills_count"] = static_cast<int64_t>(count);
}

// ============================================================================
// default-coordinator-workflow: node "plan"
// ============================================================================
// Generates a task dependency plan using the LLM (or a local heuristic).
// Inputs:  selected_skills (JSONL), package_root
// Outputs: task_plan (JSON)
extern "C" void node_plan_dependencies(const DataMap& inputs, DataMap& outputs) {
  std::string root           = resolve_root(inputs);
  std::string selected       = get_str(inputs, "selected_skills");
  std::string runtime        = root + "/runtime";
  std::string scripts        = root + "/scripts";
  std::string prompts        = root + "/prompts";

  std::filesystem::create_directories(runtime);

  // If selected_skills came via DataBus, write it to disk so plan_dependencies.sh can read it
  if(!selected.empty()) {
    write_file(runtime + "/selected_skills.jsonl", selected);
  }

  // Invoke the existing shell script (which calls LLM)
  std::string plan_json = run_cmd("'" + scripts + "/plan_dependencies.sh' 2>/dev/null && "
                                  "cat '" + runtime + "/task_plan.json' 2>/dev/null");
  if(plan_json.empty()) {
    // Fallback: build a simple linear plan from selected skills
    std::string jsonl = read_file(runtime + "/selected_skills.jsonl");
    std::ostringstream plan;
    plan << "{\"tasks\":[";
    bool first = true;
    std::istringstream ss(jsonl);
    std::string line;
    int idx = 0;
    while(std::getline(ss, line)) {
      if(line.empty()) continue;
      std::string sid   = json_get_str(line, "session_id");
      std::string skill = json_get_str(line, "skill");
      if(sid.empty()) continue;
      if(!first) plan << ",";
      plan << "{\"id\":\"task_" << idx << "\",\"session_id\":\"" << sid
           << "\",\"skill\":\"" << skill
           << "\",\"deps\":[],\"status\":\"pending\"}";
      first = false;
      ++idx;
    }
    plan << "]}";
    plan_json = plan.str();
    write_file(runtime + "/task_plan.json", plan_json);
  }

  outputs["task_plan"] = plan_json;
}

// ============================================================================
// default-coordinator-workflow: node "apply"  (COORDINATOR ORCHESTRATION)
// ============================================================================
// Evaluates the task dependency graph and makes start/stop/skip decisions.
// This node implements the core multi-session orchestration logic:
//   - Reads task_plan (JSON with tasks array)
//   - For each task, checks if all dependency tasks are done
//   - Assigns action: "start" (ready), "waiting" (deps pending), "skip" (dep failed)
//   - Updates task status records in the runtime directory
//   - Stops tasks that have exceeded their deadline or completed
// Inputs:  task_plan (JSON), selected_skills (JSONL), package_root
// Outputs: orchestration_plan (JSON)
extern "C" void node_apply_task_plan(const DataMap& inputs, DataMap& outputs) {
  std::string root    = resolve_root(inputs);
  std::string runtime = root + "/runtime";
  std::string task_plan_str = get_str(inputs, "task_plan");
  std::filesystem::create_directories(runtime);

  if(task_plan_str.empty()) {
    task_plan_str = read_file(runtime + "/task_plan.json");
  }

  // Parse tasks from the plan: collect task IDs and their deps
  // Format: {"tasks":[{"id":"task_0","session_id":"...","skill":"...","deps":[],"status":"..."},...]}
  struct TaskEntry {
    std::string id;
    std::string session_id;
    std::string skill;
    std::vector<std::string> deps;
    std::string current_status;  // pending / running / done / failed / skip
  };

  std::vector<TaskEntry> tasks;
  // Simple extraction of tasks array items
  {
    size_t tasks_pos = task_plan_str.find("\"tasks\"");
    if(tasks_pos != std::string::npos) {
      size_t arr_start = task_plan_str.find('[', tasks_pos);
      size_t arr_end   = task_plan_str.rfind(']');
      if(arr_start != std::string::npos && arr_end != std::string::npos) {
        // Iterate over objects in the array
        size_t pos = arr_start + 1;
        while(pos < arr_end) {
          size_t obj_start = task_plan_str.find('{', pos);
          if(obj_start == std::string::npos || obj_start >= arr_end) break;
          // Find matching closing brace
          size_t depth = 1;
          size_t obj_end = obj_start + 1;
          while(obj_end < task_plan_str.size() && depth > 0) {
            if(task_plan_str[obj_end] == '{') ++depth;
            else if(task_plan_str[obj_end] == '}') --depth;
            ++obj_end;
          }
          std::string obj = task_plan_str.substr(obj_start, obj_end - obj_start);
          TaskEntry e;
          e.id             = json_get_str(obj, "id");
          e.session_id     = json_get_str(obj, "session_id");
          e.skill          = json_get_str(obj, "skill");
          e.current_status = json_get_str(obj, "status");
          if(e.id.empty()) {
            pos = obj_end;
            continue;
          }
          // Check if a runtime status file exists with newer state
          std::string status_file = runtime + "/" + e.id + "_status.txt";
          if(std::filesystem::exists(status_file)) {
            std::string file_status = read_file(status_file);
            while(!file_status.empty() &&
                  (file_status.back() == '\n' || file_status.back() == '\r')) {
              file_status.pop_back();
            }
            if(!file_status.empty()) e.current_status = file_status;
          }
          tasks.push_back(std::move(e));
          pos = obj_end;
        }
      }
    }
  }

  // If no tasks parsed, fallback to building tasks from selected_skills
  if(tasks.empty()) {
    std::string jsonl = read_file(runtime + "/selected_skills.jsonl");
    if(jsonl.empty()) jsonl = get_str(inputs, "selected_skills");
    std::istringstream ss(jsonl);
    std::string line;
    int idx = 0;
    while(std::getline(ss, line)) {
      if(line.empty()) continue;
      TaskEntry e;
      e.id         = "task_" + std::to_string(idx++);
      e.session_id = json_get_str(line, "session_id");
      e.skill      = json_get_str(line, "skill");
      e.current_status = "pending";
      tasks.push_back(std::move(e));
    }
  }

  // Build a map from task_id → done/failed for dependency evaluation
  std::map<std::string, std::string> status_map;
  for(const auto& t : tasks) {
    status_map[t.id] = t.current_status;
  }

  // Orchestration decisions
  struct Decision {
    std::string task_id;
    std::string session_id;
    std::string skill;
    std::string action;   // start | waiting | skip | stop | already_done
    std::string reason;
  };
  std::vector<Decision> decisions;

  for(const auto& t : tasks) {
    Decision d;
    d.task_id    = t.id;
    d.session_id = t.session_id;
    d.skill      = t.skill;

    if(t.current_status == "done" || t.current_status == "completed") {
      d.action = "already_done";
      d.reason = "task already completed";
    } else if(t.current_status == "running") {
      // Check if it has been running too long (simple heuristic: if result exists, stop it)
      std::string result_file = runtime + "/" + t.session_id + ".skill_result.txt";
      if(std::filesystem::exists(result_file)) {
        d.action = "stop";
        d.reason = "skill result available; mark complete";
        write_file(runtime + "/" + t.id + "_status.txt", "done");
      } else {
        d.action = "waiting";
        d.reason = "task still running";
      }
    } else if(t.current_status == "failed") {
      d.action = "skip";
      d.reason = "task previously failed";
    } else {
      // Check deps
      bool all_deps_done   = true;
      bool any_dep_failed  = false;
      for(const auto& dep : t.deps) {
        auto sit = status_map.find(dep);
        if(sit == status_map.end() ||
           (sit->second != "done" && sit->second != "completed")) {
          all_deps_done = false;
        }
        if(sit != status_map.end() &&
           (sit->second == "failed" || sit->second == "skip")) {
          any_dep_failed = true;
        }
      }
      if(any_dep_failed) {
        d.action = "skip";
        d.reason = "upstream dependency failed";
        write_file(runtime + "/" + t.id + "_status.txt", "skip");
      } else if(all_deps_done) {
        d.action = "start";
        d.reason = "all dependencies satisfied";
        write_file(runtime + "/" + t.id + "_status.txt", "running");
        status_map[t.id] = "running";
      } else {
        d.action = "waiting";
        d.reason = "waiting for upstream dependencies";
      }
    }
    decisions.push_back(std::move(d));
  }

  // Serialize orchestration plan to JSON
  std::ostringstream plan;
  plan << "{\"orchestration_plan\":[";
  for(size_t i = 0; i < decisions.size(); ++i) {
    if(i > 0) plan << ",";
    const auto& d = decisions[i];
    plan << "{\"task_id\":\"" << json_esc(d.task_id)
         << "\",\"session_id\":\"" << json_esc(d.session_id)
         << "\",\"skill\":\"" << json_esc(d.skill)
         << "\",\"action\":\"" << json_esc(d.action)
         << "\",\"reason\":\"" << json_esc(d.reason) << "\"}";
  }
  plan << "]}";

  std::string plan_str = plan.str();
  write_file(runtime + "/task_plan_applied.json", plan_str);

  outputs["orchestration_plan"] = plan_str;

  // Count actions for logging
  int starts = 0, skips = 0, waits = 0;
  for(const auto& d : decisions) {
    if(d.action == "start")   ++starts;
    if(d.action == "skip")    ++skips;
    if(d.action == "waiting") ++waits;
  }
  outputs["orchestration_starts"]  = static_cast<int64_t>(starts);
  outputs["orchestration_skips"]   = static_cast<int64_t>(skips);
  outputs["orchestration_waiting"] = static_cast<int64_t>(waits);
}

// ============================================================================
// session-dispatcher-workflow: node "load_context"
// ============================================================================
// Loads the session context (history summary + prompt template).
// Inputs:  session_id, package_root
// Outputs: session_context (JSON string)
extern "C" void node_load_context(const DataMap& inputs, DataMap& outputs) {
  std::string root       = resolve_root(inputs);
  std::string session_id = get_str(inputs, "session_id", "precreated_session_1");
  std::string runtime    = root + "/runtime";
  std::string scripts    = root + "/scripts";

  std::filesystem::create_directories(runtime);

  // Invoke the session context pipeline (reads prompt, history, summarizes, fills context)
  std::string ctx_cmd =
      "AF_SESSION_ID='" + session_id + "' '" + scripts +
      "/session_context_pipeline.sh' 2>/dev/null";
  run_cmd(ctx_cmd);

  // Read the produced context file
  std::string ctx_file = runtime + "/" + session_id + ".context.json";
  std::string ctx      = read_file(ctx_file);
  if(ctx.empty()) {
    ctx = "{\"session_id\":\"" + session_id + "\",\"context\":{}}";
  }
  outputs["session_context"] = ctx;
}

// ============================================================================
// session-dispatcher-workflow: node "select_skill"
// ============================================================================
// Uses LLM (or heuristic fallback) to select the best skill for this session.
// Inputs:  session_id, user_text, session_context, package_root
// Outputs: skill_name, skill_params
extern "C" void node_select_skill(const DataMap& inputs, DataMap& outputs) {
  std::string root        = resolve_root(inputs);
  std::string session_id  = get_str(inputs, "session_id", "precreated_session_1");
  std::string user_text   = get_str(inputs, "user_text");
  std::string runtime     = root + "/runtime";
  std::string scripts     = root + "/scripts";

  std::filesystem::create_directories(runtime);

  if(!user_text.empty()) {
    write_file(runtime + "/latest_user_input.txt", user_text);
  }

  std::string select_cmd =
      "AF_SESSION_ID='" + session_id + "' AF_INPUT='user=" + json_esc(user_text) +
      "' '" + scripts + "/select_skill.sh' 2>/dev/null";
  run_cmd(select_cmd);

  std::string skill_file = runtime + "/" + session_id + ".skill.json";
  std::string skill_json = read_file(skill_file);
  std::string skill_name = json_get_str(skill_json, "skill");
  if(skill_name.empty()) skill_name = "skill_search";

  outputs["skill_name"]   = skill_name;
  outputs["skill_params"] = skill_json;
  outputs["skill_json"]   = skill_json;
}

// ============================================================================
// session-dispatcher-workflow: node "create_task"
// ============================================================================
// Creates a task record for the selected skill (status: created_not_started).
// The coordinator's apply node will later decide when to start/stop it.
// Inputs:  session_id, skill_name, skill_params, package_root
// Outputs: task_id, task_status
extern "C" void node_create_task(const DataMap& inputs, DataMap& outputs) {
  std::string root       = resolve_root(inputs);
  std::string session_id = get_str(inputs, "session_id", "precreated_session_1");
  std::string skill_json = get_str(inputs, "skill_params");
  std::string runtime    = root + "/runtime";

  std::filesystem::create_directories(runtime);

  std::string task_id = session_id + "_skill_task";

  if(skill_json.empty()) {
    std::string skill_name = get_str(inputs, "skill_name", "skill_search");
    skill_json = "{\"session_id\":\"" + session_id + "\",\"skill\":\"" + skill_name + "\"}";
  }

  std::string task_json =
      "{\"task_id\":\"" + task_id +
      "\",\"status\":\"created_not_started\",\"skill\":" + skill_json + "}";
  write_file(runtime + "/" + task_id + ".json", task_json);
  write_file(runtime + "/" + task_id + "_status.txt", "created_not_started");

  outputs["task_id"]     = task_id;
  outputs["task_status"] = std::string("created_not_started");
  outputs["task_json"]   = task_json;
}
