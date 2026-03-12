// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "../../features/agent_tui_main.h"

int main(int argc, char* argv[]) {
  agentflow::examples::SimpleDispatcherTuiOptions options;
  options.default_package_path = "examples/agent/openjiuwen_studio_agent";
  options.log_file_name = "openjiuwen_studio_agent_tui.log";
  options.title = "OpenJiuwen Studio Agent TUI";
  options.dispatcher_workflow = "openjiuwen-studio-dispatcher-workflow";
  options.initialize_error_message = "Error: failed to initialize OpenJiuwen Studio Agent runner";
  options.timeout_error_message = "timeout waiting for OpenJiuwen Studio Agent response";
  options.bye_message = "Bye from OpenJiuwen Studio Agent.";
  options.help_lines = {
      "  /help   Show this help",
      "  /quit   Exit the TUI",
      "  /status Ask the bundled Studio agent for bundle status",
      "  any other text will be sent to openjiuwen-studio-dispatcher-workflow",
  };
  return agentflow::examples::run_simple_dispatcher_tui(argc, argv, options);
}
