// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "../../features/agent_tui_main.h"

int main(int argc, char* argv[]) {
  agentflow::examples::SimpleDispatcherTuiOptions options;
  options.default_package_path = "examples/agent/llm_query_agent";
  options.log_file_name = "llm_query_agent_tui.log";
  options.title = "LLM Query Agent TUI";
  options.dispatcher_workflow = "llm-query-dispatcher-workflow";
  options.initialize_error_message = "Error: failed to initialize LLM Query Agent runner";
  options.timeout_error_message = "timeout waiting for LLM Query Agent response";
  options.bye_message = "Bye from LLM Query Agent.";
  options.timeout_ms = 120000;
  options.help_lines = {
      "  /help   Show this help",
      "  /quit   Exit the TUI",
      "  /status Ask the LLM Query Agent for runtime status",
      "  any other text will be sent to llm-query-dispatcher-workflow",
  };
  return agentflow::examples::run_simple_dispatcher_tui(argc, argv, options);
}
