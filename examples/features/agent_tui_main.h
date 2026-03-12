// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#ifndef AGENTFLOW_EXAMPLES_AGENT_TUI_MAIN_H
#define AGENTFLOW_EXAMPLES_AGENT_TUI_MAIN_H

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "chat_bridge_support.h"
#include "interactive_runner.h"
#include "tui_support.h"

namespace agentflow::examples {

struct SimpleDispatcherTuiOptions {
  std::string default_package_path;
  std::string log_file_name;
  std::string title;
  std::string dispatcher_workflow;
  std::string initialize_error_message;
  std::string timeout_error_message;
  std::string bye_message;
  int timeout_ms = 10000;
  std::vector<std::string> help_lines;
};

struct ChatSessionTuiOptions {
  std::string default_package_path;
  std::string log_file_name;
  std::string title;
  std::string dispatcher_workflow;
  std::string initialize_error_message;
  std::string bye_message;
  std::string session_id = "tui:default";
  std::string history_dir = "history";
  std::vector<std::string> help_lines;
};

inline std::string resolve_tui_package_path(int argc, char* argv[],
                                            const std::string& default_package_path) {
  if(argc > 1) {
    return argv[1];
  }
  return default_package_path;
}

inline void write_help(TuiConsole& ui, const std::vector<std::string>& help_lines) {
  ui.write_line("Commands:");
  for(const auto& line : help_lines) {
    ui.write_line(line);
  }
}

inline bool prepare_tui_environment(const std::filesystem::path& package_path,
                                    TuiConsole* ui,
                                    std::filesystem::path* original_cwd) {
  namespace fs = std::filesystem;
  if(ui == nullptr || original_cwd == nullptr) {
    return false;
  }
  if(!fs::exists(package_path / "package.json")) {
    std::cerr << "Error: package.json not found under " << package_path << std::endl;
    return false;
  }

  *original_cwd = fs::current_path();
  std::string redirect_error;
  if(!ui->redirect_process_logs(&redirect_error)) {
    std::cerr << "Error: failed to isolate TUI logs: " << redirect_error << std::endl;
    return false;
  }

  fs::current_path(package_path);
  return true;
}

inline int run_simple_dispatcher_tui(int argc, char* argv[],
                                     const SimpleDispatcherTuiOptions& options) {
  namespace fs = std::filesystem;

  fs::path package_path = fs::absolute(resolve_tui_package_path(argc, argv, options.default_package_path));
  TuiConsole ui(package_path / "runtime" / "logs" / options.log_file_name);
  fs::path original_cwd;
  if(!prepare_tui_environment(package_path, &ui, &original_cwd)) {
    return 1;
  }

  InteractiveRunner::Config config;
  config.timeout_ms = options.timeout_ms;
  InteractiveRunner runner(".", config);
  if(!runner.initialize()) {
    ui.write_line(options.initialize_error_message);
    fs::current_path(original_cwd);
    return 1;
  }

  ui.write_line("=== " + options.title + " ===");
  ui.write_line("Package: " + package_path.string());
  ui.write_line("Dispatcher workflow: " + options.dispatcher_workflow);
  ui.write_line("Runtime logs: " + ui.log_path().string());
  ui.write_line("Type /help or /quit.");
  ui.write_blank_line();
  write_help(ui, options.help_lines);
  ui.write_blank_line();

  std::string line;
  while(read_tui_input(ui, &line)) {
    if(line.empty()) {
      continue;
    }
    if(line == "/quit" || line == "quit" || line == "exit") {
      ui.write_line(options.bye_message);
      break;
    }
    if(line == "/help" || line == "help") {
      write_help(ui, options.help_lines);
      ui.write_blank_line();
      continue;
    }

    const std::string response = runner.execute_request(options.dispatcher_workflow, line);
    if(response.empty()) {
      ui.write_line("assistant> [" + options.timeout_error_message + "]");
      ui.write_blank_line();
      continue;
    }

    ui.write_prefixed_lines("assistant> ", response, "[empty response]");
    ui.write_blank_line();
  }

  fs::current_path(original_cwd);
  return 0;
}

inline int run_chat_session_tui(int argc, char* argv[], const ChatSessionTuiOptions& options) {
  namespace fs = std::filesystem;

  fs::path package_path = fs::absolute(resolve_tui_package_path(argc, argv, options.default_package_path));
  TuiConsole ui(package_path / "runtime" / "logs" / options.log_file_name);
  fs::path original_cwd;
  if(!prepare_tui_environment(package_path, &ui, &original_cwd)) {
    return 1;
  }

  ChatAgentBridge bridge(".");
  if(!bridge.initialize()) {
    ui.write_line(options.initialize_error_message);
    fs::current_path(original_cwd);
    return 1;
  }
  ConversationStore store(options.history_dir);

  ui.write_line("=== " + options.title + " ===");
  ui.write_line("Package: " + package_path.string());
  ui.write_line("Dispatcher workflow: " + options.dispatcher_workflow);
  ui.write_line("Runtime logs: " + ui.log_path().string());
  ui.write_line("Type /help or /quit.");
  ui.write_blank_line();
  write_help(ui, options.help_lines);
  ui.write_blank_line();

  std::string line;
  while(read_tui_input(ui, &line)) {
    if(line.empty()) {
      continue;
    }
    if(line == "/quit" || line == "quit" || line == "exit") {
      ui.write_line(options.bye_message);
      break;
    }
    if(line == "/help" || line == "help") {
      write_help(ui, options.help_lines);
      ui.write_blank_line();
      continue;
    }
    if(line == "/status" || line == "status") {
      ui.write_line("status> package: " + package_path.string());
      ui.write_line("status> session: " + options.session_id);
      ui.write_line("status> history file: " + store.session_file(options.session_id).string());
      ui.write_line("status> runtime logs: " + ui.log_path().string());
      ui.write_blank_line();
      continue;
    }

    store.append(options.session_id, "user", line);
    const std::string history_json = store.recent_as_json(options.session_id, 12);
    std::string response = build_conversation_response(line, history_json);
    if(response.empty()) {
      const std::string payload =
          "session=" + options.session_id + "\nuser=" + line + "\nhistory=" + history_json;
      response = bridge.execute(options.dispatcher_workflow, payload);
    }
    store.append(options.session_id, "assistant", response);
    ui.write_prefixed_lines("assistant> ", response, "[empty response]");
    ui.write_blank_line();
  }

  fs::current_path(original_cwd);
  return 0;
}

}  // namespace agentflow::examples

#endif  // AGENTFLOW_EXAMPLES_AGENT_TUI_MAIN_H
