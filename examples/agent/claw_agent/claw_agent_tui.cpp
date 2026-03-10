// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <filesystem>
#include <iostream>
#include <string>

#include "../../features/interactive_runner.h"
#include "../../features/tui_support.h"

namespace claw_tui {

void print_help(agentflow::examples::TuiConsole& ui) {
  ui.write_line("Commands:");
  ui.write_line("  /help   Show this help");
  ui.write_line("  /quit   Exit the TUI");
  ui.write_line("  /status Ask ClawAgent for runtime status");
  ui.write_line("  any other text will be sent to claw-dispatcher-workflow");
}

std::string resolve_package_path(int argc, char* argv[]) {
  if(argc > 1) {
    return argv[1];
  }
  return "examples/agent/claw_agent";
}

}  // namespace claw_tui

int main(int argc, char* argv[]) {
  namespace fs = std::filesystem;
  using agentflow::examples::TuiConsole;
  using agentflow::examples::trim_text;

  const fs::path original_cwd = fs::current_path();
  fs::path package_path = fs::absolute(claw_tui::resolve_package_path(argc, argv));
  if(!fs::exists(package_path / "package.json")) {
    std::cerr << "Error: package.json not found under " << package_path << std::endl;
    return 1;
  }

  TuiConsole ui(package_path / "runtime" / "logs" / "claw_agent_tui.log");
  std::string redirect_error;
  if(!ui.redirect_process_logs(&redirect_error)) {
    std::cerr << "Error: failed to isolate TUI logs: " << redirect_error << std::endl;
    return 1;
  }

  fs::current_path(package_path);

  agentflow::InteractiveRunner::Config config;
  config.timeout_ms = 10000;
  agentflow::InteractiveRunner runner(".", config);
  if(!runner.initialize()) {
    ui.write_line("Error: failed to initialize ClawAgent runner");
    fs::current_path(original_cwd);
    return 1;
  }

  ui.write_line("=== ClawAgent TUI ===");
  ui.write_line("Package: " + package_path.string());
  ui.write_line("Dispatcher workflow: claw-dispatcher-workflow");
  ui.write_line("Runtime logs: " + ui.log_path().string());
  ui.write_line("Type /help or /quit.");
  ui.write_blank_line();
  claw_tui::print_help(ui);
  ui.write_blank_line();

  std::string line;
  while(std::getline(std::cin, line)) {
    line = trim_text(line);
    if(line.empty()) {
      continue;
    }
    if(line == "/quit" || line == "quit" || line == "exit") {
      ui.write_line("Bye from ClawAgent.");
      break;
    }
    if(line == "/help" || line == "help") {
      claw_tui::print_help(ui);
      ui.write_blank_line();
      continue;
    }

    ui.write_line("you> " + line);
    const std::string response = runner.execute_request("claw-dispatcher-workflow", line);
    if(response.empty()) {
      ui.write_line("assistant> [timeout waiting for ClawAgent response]");
      ui.write_blank_line();
      continue;
    }

    ui.write_prefixed_lines("assistant> ", response, "[empty response]");
    ui.write_blank_line();
  }

  fs::current_path(original_cwd);
  return 0;
}
