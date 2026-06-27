// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// harness_runner — self-contained driver for the harness_agent example.
//
// Drives the harness agent loop using the AgentFlow runtime's workflow engine
// (runtime->execute_workflow), sequencing the coordinator + skill workflows the
// same way examples/features/chat_bridge_support.h drives multi_session_chat.
// Driver orchestration avoids async-node races and the framework's premature
// dialogue cleanup, giving a deterministic, testable harness loop.
//
// The agent package (agent.json) defines the full dispatcher/coordinator/skill
// agent; this driver plays the dispatcher's entry role (first skill) and relies
// on the coordinator workflow's decide_next core for every subsequent step and
// for termination — exactly the harness loop semantics.
//
// Lives entirely inside examples/agent/harness_agent; compiled standalone by
// scripts/build_and_run.sh against the prebuilt libagentflow. No AgentFlow
// source or root CMake changes.
//
// Usage:
//   harness_runner <package_path> [--input "text"] [--session id] [--timeout s]
//   printf 'hello\n/quit\n' | harness_runner <package_path>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "agent/agent.h"
#include "runtime/agent_runtime.h"

namespace fs = std::filesystem;
using namespace agentflow;

namespace {

std::atomic<int> g_seq{0};

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string sanitize(const std::string& p) {
  std::string out;
  for (char c : p) out += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
  return out;
}

std::string read_file(const fs::path& p) {
  std::ifstream f(p);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Wait until a marker file exists and (optionally) its trimmed content equals
// `expect`. Returns content, or "" on timeout.
std::string wait_marker(const fs::path& path, const std::string& expect,
                        int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fs::exists(path)) {
      const std::string content = trim(read_file(path));
      if (expect.empty() || content == expect) return content;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  return "";
}

struct Markers {
  fs::path decision, response, skilldone;
};

Markers marker_paths(const fs::path& pkg, const std::string& session,
                     const std::string& dialogue) {
  fs::path base = pkg / "runtime" / "markers";
  std::string stem = sanitize(session) + "__" + sanitize(dialogue);
  return {base / (stem + ".decision"), base / (stem + ".response"),
          base / (stem + ".skilldone")};
}

DataMap make_inputs(const std::string& user_text,
                               const std::string& session,
                               const std::string& dialogue,
                               const std::string& completed_skill) {
  DataMap inputs;
  inputs["user_text"] = DataVariant(user_text);
  inputs["session_id"] = DataVariant(session);
  inputs["dialogue_id"] = DataVariant(dialogue);
  if (!completed_skill.empty())
    inputs["completed_skill_name"] = DataVariant(completed_skill);
  return inputs;
}

void wait_idle(runtime::MMAgentRuntime* rt, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  // Allow scheduling to start, then wait for nodes to drain.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  while (std::chrono::steady_clock::now() < deadline) {
    if (rt->get_running_nodes() == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
}

std::string run_once(std::shared_ptr<framework::MMAgent>& agent,
                     runtime::MMAgentRuntime* rt, const fs::path& pkg,
                     const std::string& user_text, const std::string& session,
                     int timeout_sec) {
  const std::string dialogue =
      "d" + std::to_string(++g_seq) + "_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string agent_id = agent->get_id();
  Markers mk = marker_paths(pkg, session, dialogue);
  std::error_code ec;
  fs::remove(mk.decision, ec);
  fs::remove(mk.response, ec);
  fs::remove(mk.skilldone, ec);

  const int step_ms = timeout_sec * 1000;

  // Context assembly before model access (requirement #2).
  rt->execute_workflow(agent_id, "harness-context-workflow",
                       make_inputs(user_text, session, dialogue, ""), "",
                       session, dialogue);
  wait_idle(rt, step_ms);

  // Dispatcher entry: first skill is retrieve.
  std::string selected = "retrieve";
  std::string response;
  const int max_steps = 12;
  for (int step = 0; step < max_steps; ++step) {
    // Run the selected skill workflow.
    fs::remove(mk.skilldone, ec);
    rt->execute_workflow(agent_id, "harness-skill-" + selected + "-workflow",
                         make_inputs(user_text, session, dialogue, ""), "",
                         session, dialogue);
    wait_marker(mk.skilldone, selected, step_ms);
    wait_idle(rt, step_ms);

    // Run the coordinator to decide the next step (or terminate).
    fs::remove(mk.decision, ec);
    rt->execute_workflow(agent_id, "harness-coordinator-workflow",
                         make_inputs(user_text, session, dialogue, selected), "",
                         session, dialogue);
    const std::string decision = wait_marker(mk.decision, "", step_ms);
    wait_idle(rt, step_ms);

    if (decision.rfind("NEXT:", 0) == 0) {
      selected = trim(decision.substr(5));
      continue;
    }
    // DONE (or timeout): read the final response.
    response = trim(read_file(mk.response));
    break;
  }
  return response;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: harness_runner <package_path> [--input \"text\"] "
                 "[--session id] [--timeout s]\n";
    return 2;
  }
  std::string package_path = argv[1];
  std::string single_input, session = "default";
  int timeout_sec = 20;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--input" && i + 1 < argc) single_input = argv[++i];
    else if (a == "--session" && i + 1 < argc) session = argv[++i];
    else if (a == "--timeout" && i + 1 < argc)
      timeout_sec = std::max(5, std::atoi(argv[++i]));
  }
  fs::path pkg = fs::absolute(package_path);

  auto agent = framework::MMAgent::create_from_package(package_path);
  if (!agent || !agent->initialize()) {
    std::cerr << "HARNESS_ERROR: failed to load package: " << package_path << "\n";
    return 1;
  }
  auto runtime = std::make_unique<runtime::MMAgentRuntime>();
  if (!runtime->register_agent(agent) || !runtime->start() || !agent->start()) {
    std::cerr << "HARNESS_ERROR: failed to start runtime/agent\n";
    return 1;
  }

  std::cout << "Harness Agent Runner ready (package=" << package_path << ")\n";
  std::cout.flush();

  auto handle = [&](const std::string& text) {
    const std::string response =
        run_once(agent, runtime.get(), pkg, text, session, timeout_sec);
    std::cout << "HARNESS_RESPONSE: "
              << (response.empty() ? "[no response]" : response) << "\n";
    std::cout.flush();
  };

  if (!single_input.empty()) {
    handle(single_input);
  } else {
    std::string line;
    while (std::getline(std::cin, line)) {
      line = trim(line);
      if (line.empty()) continue;
      if (line == "/quit" || line == "/exit" || line == "quit" || line == "exit")
        break;
      handle(line);
    }
  }

  std::cout << "Harness Agent Runner bye.\n";
  std::cout.flush();
  runtime->stop();
  agent->stop();
  // Work is complete; bypass crashing global/teardown destructors with an
  // immediate clean exit (threads already joined via stop()).
  std::cout.flush();
  std::fflush(nullptr);
  std::_Exit(0);
}
