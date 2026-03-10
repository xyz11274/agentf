// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <filesystem>
#include <iostream>
#include <string>

#include "../../features/chat_bridge_support.h"
#include "../../features/tui_support.h"

namespace multi_session_tui {

void print_help(agentflow::examples::TuiConsole& ui) {
  ui.write_line("Commands:");
  ui.write_line("  /help   Show this help");
  ui.write_line("  /quit   Exit the TUI");
  ui.write_line("  /status Show local TUI status");
  ui.write_line("  any other text will be sent to session-dispatcher-workflow");
}

std::string resolve_package_path(int argc, char* argv[]) {
  if(argc > 1) {
    return argv[1];
  }
  return "examples/agent/multi_session_chat/package";
}

}  // namespace multi_session_tui

int main(int argc, char* argv[]) {
  namespace fs = std::filesystem;
  using agentflow::examples::ChatAgentBridge;
  using agentflow::examples::ConversationStore;
  using agentflow::examples::TuiConsole;
  using agentflow::examples::build_conversation_response;
  using agentflow::examples::read_tui_input;

  const std::string session_id = "tui:default";
  const fs::path original_cwd = fs::current_path();
  fs::path package_path = fs::absolute(multi_session_tui::resolve_package_path(argc, argv));
  if(!fs::exists(package_path / "package.json")) {
    std::cerr << "Error: package.json not found under " << package_path << std::endl;
    return 1;
  }

  TuiConsole ui(package_path / "runtime" / "logs" / "multi_session_chat_tui.log");
  std::string redirect_error;
  if(!ui.redirect_process_logs(&redirect_error)) {
    std::cerr << "Error: failed to isolate TUI logs: " << redirect_error << std::endl;
    return 1;
  }

  fs::current_path(package_path);

  ChatAgentBridge bridge(".");
  if(!bridge.initialize()) {
    ui.write_line("Error: failed to initialize multi_session_chat bridge");
    fs::current_path(original_cwd);
    return 1;
  }
  ConversationStore store("history");

  ui.write_line("=== Multi-Session Chat TUI ===");
  ui.write_line("Package: " + package_path.string());
  ui.write_line("Dispatcher workflow: session-dispatcher-workflow");
  ui.write_line("Runtime logs: " + ui.log_path().string());
  ui.write_line("Type /help or /quit.");
  ui.write_blank_line();
  multi_session_tui::print_help(ui);
  ui.write_blank_line();

  std::string line;
  while(read_tui_input(ui, &line)) {
    if(line.empty()) {
      continue;
    }
    if(line == "/quit" || line == "quit" || line == "exit") {
      ui.write_line("Bye from multi_session_chat.");
      break;
    }
    if(line == "/help" || line == "help") {
      multi_session_tui::print_help(ui);
      ui.write_blank_line();
      continue;
    }
    if(line == "/status" || line == "status") {
      ui.write_line("status> package: " + package_path.string());
      ui.write_line("status> session: " + session_id);
      ui.write_line("status> history file: " + store.session_file(session_id).string());
      ui.write_line("status> runtime logs: " + ui.log_path().string());
      ui.write_blank_line();
      continue;
    }
    store.append(session_id, "user", line);
    const std::string history_json = store.recent_as_json(session_id, 12);
    std::string response = build_conversation_response(line, history_json);
    if(response.empty()) {
      const std::string payload =
          "session=" + session_id + "\nuser=" + line + "\nhistory=" + history_json;
      response = bridge.execute("session-dispatcher-workflow", payload);
    }
    store.append(session_id, "assistant", response);
    ui.write_prefixed_lines("assistant> ", response, "[empty response]");
    ui.write_blank_line();
  }

  fs::current_path(original_cwd);
  return 0;
}
