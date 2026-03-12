// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "../../features/agent_tui_main.h"

int main(int argc, char* argv[]) {
  agentflow::examples::SimpleDispatcherTuiOptions options;
  options.default_package_path = "examples/agent/claw_agent";
  options.log_file_name = "claw_agent_tui.log";
  options.title = "ClawAgent TUI";
  options.dispatcher_workflow = "claw-dispatcher-workflow";
  options.initialize_error_message = "Error: failed to initialize ClawAgent runner";
  options.timeout_error_message = "timeout waiting for ClawAgent response";
  options.bye_message = "Bye from ClawAgent.";
  options.help_lines = {
      "  /help   Show this help",
      "  /quit   Exit the TUI",
      "  /status Ask ClawAgent for runtime status",
      "  any other text will be sent to claw-dispatcher-workflow",
  };
  return agentflow::examples::run_simple_dispatcher_tui(argc, argv, options);
}
