// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "../../features/agent_tui_main.h"

int main(int argc, char* argv[]) {
  agentflow::examples::ChatSessionTuiOptions options;
  options.default_package_path = "examples/agent/multi_session_chat/package";
  options.log_file_name = "multi_session_chat_tui.log";
  options.title = "Multi-Session Chat TUI";
  options.dispatcher_workflow = "session-dispatcher-workflow";
  options.initialize_error_message = "Error: failed to initialize multi_session_chat bridge";
  options.bye_message = "Bye from multi_session_chat.";
  options.help_lines = {
      "  /help   Show this help",
      "  /quit   Exit the TUI",
      "  /status Show local TUI status",
      "  any other text will be sent to session-dispatcher-workflow",
  };
  return agentflow::examples::run_chat_session_tui(argc, argv, options);
}
