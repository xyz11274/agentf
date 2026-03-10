// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <filesystem>
#include <iostream>
#include <string>

#include "../../features/interactive_runner.h"
#include "../../features/tui_support.h"

namespace llm_query_tui {

void print_help(agentflow::examples::TuiConsole& ui) {
  ui.write_line("Commands:");
  ui.write_line("  /help   Show this help");
  ui.write_line("  /quit   Exit the TUI");
  ui.write_line("  /status Ask the LLM Query Agent for runtime status");
  ui.write_line("  any other text will be sent to llm-query-dispatcher-workflow");
}

std::string resolve_package_path(int argc, char* argv[]) {
  if(argc > 1) {
    return argv[1];
  }
  return "examples/agent/llm_query_agent";
}

}  // namespace llm_query_tui

int main(int argc, char* argv[]) {
  namespace fs = std::filesystem;
  using agentflow::examples::TuiConsole;
  using agentflow::examples::read_tui_input;

  const fs::path original_cwd = fs::current_path();
  fs::path package_path = fs::absolute(llm_query_tui::resolve_package_path(argc, argv));
  if(!fs::exists(package_path / "package.json")) {
    std::cerr << "Error: package.json not found under " << package_path << std::endl;
    return 1;
  }

  TuiConsole ui(package_path / "runtime" / "logs" / "llm_query_agent_tui.log");
  std::string redirect_error;
  if(!ui.redirect_process_logs(&redirect_error)) {
    std::cerr << "Error: failed to isolate TUI logs: " << redirect_error << std::endl;
    return 1;
  }

  fs::current_path(package_path);

  agentflow::InteractiveRunner::Config config;
  config.timeout_ms = 120000;
  agentflow::InteractiveRunner runner(".", config);
  if(!runner.initialize()) {
    ui.write_line("Error: failed to initialize LLM Query Agent runner");
    fs::current_path(original_cwd);
    return 1;
  }

  ui.write_line("=== LLM Query Agent TUI ===");
  ui.write_line("Package: " + package_path.string());
  ui.write_line("Dispatcher workflow: llm-query-dispatcher-workflow");
  ui.write_line("Runtime logs: " + ui.log_path().string());
  ui.write_line("Type /help or /quit.");
  ui.write_blank_line();
  llm_query_tui::print_help(ui);
  ui.write_blank_line();

  std::string line;
  while(read_tui_input(ui, &line)) {
    if(line.empty()) {
      continue;
    }
    if(line == "/quit" || line == "quit" || line == "exit") {
      ui.write_line("Bye from LLM Query Agent.");
      break;
    }
    if(line == "/help" || line == "help") {
      llm_query_tui::print_help(ui);
      ui.write_blank_line();
      continue;
    }

    const std::string response = runner.execute_request("llm-query-dispatcher-workflow", line);
    if(response.empty()) {
      ui.write_line("assistant> [timeout waiting for LLM Query Agent response]");
      ui.write_blank_line();
      continue;
    }

    ui.write_prefixed_lines("assistant> ", response, "[empty response]");
    ui.write_blank_line();
  }

  fs::current_path(original_cwd);
  return 0;
}
