// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <filesystem>
#include <iostream>
#include <string>

#include "../../features/interactive_runner.h"

namespace studio_tui {

std::string trim(const std::string& input) {
  const auto start = input.find_first_not_of(" \t\r\n");
  if(start == std::string::npos) {
    return "";
  }
  const auto end = input.find_last_not_of(" \t\r\n");
  return input.substr(start, end - start + 1);
}

void print_help() {
  std::cout << "Commands:\n"
            << "  /help   Show this help\n"
            << "  /quit   Exit the TUI\n"
            << "  /status Ask the bundled Studio agent for bundle status\n"
            << "  any other text will be sent to openjiuwen-studio-dispatcher-workflow\n";
}

std::string resolve_package_path(int argc, char* argv[]) {
  if(argc > 1) {
    return argv[1];
  }
  return "examples/agent/openjiuwen_studio_agent";
}

}  // namespace studio_tui

int main(int argc, char* argv[]) {
  namespace fs = std::filesystem;

  const fs::path original_cwd = fs::current_path();
  fs::path package_path = fs::absolute(studio_tui::resolve_package_path(argc, argv));
  if(!fs::exists(package_path / "package.json")) {
    std::cerr << "Error: package.json not found under " << package_path << std::endl;
    return 1;
  }

  fs::current_path(package_path);

  agentflow::InteractiveRunner::Config config;
  config.timeout_ms = 10000;
  agentflow::InteractiveRunner runner(".", config);
  if(!runner.initialize()) {
    std::cerr << "Error: failed to initialize OpenJiuwen Studio Agent runner" << std::endl;
    fs::current_path(original_cwd);
    return 1;
  }

  std::cout << "=== OpenJiuwen Studio Agent TUI ===\n"
            << "Package: " << package_path.string() << "\n"
            << "Dispatcher workflow: openjiuwen-studio-dispatcher-workflow\n"
            << "Type /help or /quit.\n"
            << std::endl;
  studio_tui::print_help();
  std::cout << std::endl;

  std::string line;
  while(std::getline(std::cin, line)) {
    line = studio_tui::trim(line);
    if(line.empty()) {
      continue;
    }
    if(line == "/quit" || line == "quit" || line == "exit") {
      std::cout << "Bye from OpenJiuwen Studio Agent." << std::endl;
      break;
    }
    if(line == "/help" || line == "help") {
      studio_tui::print_help();
      std::cout << std::endl;
      continue;
    }

    std::cout << "you> " << line << std::endl;
    const std::string response = runner.execute_request("openjiuwen-studio-dispatcher-workflow", line);
    if(response.empty()) {
      std::cout << "assistant> [timeout waiting for OpenJiuwen Studio Agent response]" << std::endl;
      std::cout << std::endl;
      continue;
    }

    size_t cursor = 0;
    bool printed = false;
    while(cursor <= response.size()) {
      size_t next = response.find('\n', cursor);
      const std::string chunk =
          next == std::string::npos ? response.substr(cursor) : response.substr(cursor, next - cursor);
      if(!chunk.empty()) {
        std::cout << "assistant> " << chunk << std::endl;
        printed = true;
      }
      if(next == std::string::npos) {
        break;
      }
      cursor = next + 1;
    }
    if(!printed) {
      std::cout << "assistant> [empty response]" << std::endl;
    }
    std::cout << std::endl;
  }

  fs::current_path(original_cwd);
  return 0;
}
