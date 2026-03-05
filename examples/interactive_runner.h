// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// InteractiveRunner - Message-driven interactive execution for MMAgent packages

#ifndef AGENTFLOW_EXAMPLES_INTERACTIVE_RUNNER_H
#define AGENTFLOW_EXAMPLES_INTERACTIVE_RUNNER_H

#include <chrono>
#include <memory>
#include <string>

#include "agent/agent.h"
#include "runtime/agent_runtime.h"

namespace agentflow {

/**
 * InteractiveRunner provides a message-driven interactive execution environment
 * for MMAgent packages. It routes user input through the framework's message queue
 * instead of directly executing nodes, ensuring proper use of:
 * - Session management
 * - Coordinator task coordination
 * - MMAgentRuntime three-tier scheduling
 * - MMWorkflow DAG dependency handling
 */
class InteractiveRunner {
public:
  struct Config {
    int timeout_ms = 30000;
    bool verbose = false;
  };

  explicit InteractiveRunner(const std::string& package_path);
  InteractiveRunner(const std::string& package_path, const Config& config);
  ~InteractiveRunner();

  // Non-copyable
  InteractiveRunner(const InteractiveRunner&) = delete;
  InteractiveRunner& operator=(const InteractiveRunner&) = delete;

  /**
   * Initialize the runner by loading the agent package and runtime
   * @return true if initialization succeeded
   */
  bool initialize();

  /**
   * Run the interactive command loop
   * Reads commands from stdin and processes them through the message-driven pipeline
   */
  void run();

  /**
   * Execute a single workflow with given input (non-interactive mode)
   * @param workflow_id The workflow to execute
   * @param input The input content
   * @return Output from workflow execution
   */
  std::string execute_workflow(const std::string& workflow_id, const std::string& input);

  /**
   * List all available workflows in the loaded package
   * @return Formatted string listing workflows
   */
  std::string list_workflows() const;

  /**
   * Get the status of the runner and loaded agent
   * @return Formatted status string
   */
  std::string get_status() const;

private:
  // Core components
  std::string package_path_;
  Config config_;
  std::shared_ptr<framework::MMAgent> agent_;
  std::unique_ptr<runtime::MMAgentRuntime> runtime_;

  // Message ID counter for generating unique IDs
  int64_t message_counter_ = 0;

  /**
   * Submit a user request through the message-driven pipeline
   * Routes the message through MMAgent::route_request_message()
   */
  void submit_user_request(const std::string& workflow_id, const std::string& input);

  /**
   * Process all pending messages and generated tasks
   * Uses MMAgent::process_all_sessions_until_idle() and schedules tasks
   */
  void process_pending_tasks();

  /**
   * Wait for workflow execution to complete
   * @param timeout_ms Maximum time to wait
   * @return true if execution completed within timeout
   */
  bool wait_for_completion(int timeout_ms);

  /**
   * Collect results from session send_queue
   * @return Collected output string
   */
  std::string collect_results();

  /**
   * Generate a unique message ID
   */
  std::string generate_message_id();

  /**
   * Print help message for interactive mode
   */
  void print_help() const;

  /**
   * Parse and execute a command string
   * @param command_line The command line to parse
   * @return true if should continue, false if should exit
   */
  bool handle_command(const std::string& command_line);
};

}  // namespace agentflow

#endif  // AGENTFLOW_EXAMPLES_INTERACTIVE_RUNNER_H