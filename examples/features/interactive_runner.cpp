// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// InteractiveRunner - Message-driven interactive execution for MMAgent packages

#include "interactive_runner.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

#include "agent/package.h"
#include "agent/session.h"
#include "agent/task.h"
#include "runtime/databus/databus.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {

namespace {
std::string trim(const std::string& str) {
  size_t start = str.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = str.find_last_not_of(" \t\n\r");
  return str.substr(start, end - start + 1);
}

std::vector<std::string> split_args(const std::string& line) {
  std::vector<std::string> args;
  std::string current;
  bool in_quotes = false;
  for (char c : line) {
    if (c == '"') {
      in_quotes = !in_quotes;
    } else if ((c == ' ' || c == '\t') && !in_quotes) {
      if (!current.empty()) {
        args.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    args.push_back(current);
  }
  return args;
}
}  // namespace

InteractiveRunner::InteractiveRunner(const std::string& package_path)
    : package_path_(package_path) {
}

InteractiveRunner::InteractiveRunner(const std::string& package_path, const Config& config)
    : package_path_(package_path), config_(config) {
}

InteractiveRunner::~InteractiveRunner() {
  if (runtime_) {
    runtime_->stop();
  }
  if (agent_) {
    agent_->stop();
  }
}

bool InteractiveRunner::initialize() {
  // Resolve package path
  if (!std::filesystem::exists(package_path_)) {
    std::cerr << "Error: Package path does not exist: " << package_path_ << std::endl;
    return false;
  }

  if (!std::filesystem::exists(package_path_ + "/package.json")) {
    std::cerr << "Error: No package.json found at: " << package_path_ << std::endl;
    return false;
  }

  // Create and initialize MMAgent from package
  agent_ = framework::MMAgent::create_from_package(package_path_);
  if (!agent_) {
    std::cerr << "Error: Failed to create agent from package" << std::endl;
    return false;
  }

  if (!agent_->initialize()) {
    std::cerr << "Error: Failed to initialize agent" << std::endl;
    return false;
  }

  // Create and start MMAgentRuntime
  runtime_ = std::make_unique<runtime::MMAgentRuntime>();
  if (!runtime_->register_agent(agent_)) {
    std::cerr << "Error: Failed to register agent with runtime" << std::endl;
    return false;
  }

  if (!runtime_->start()) {
    std::cerr << "Error: Failed to start runtime" << std::endl;
    return false;
  }

  // Start the agent event loop
  if (!agent_->start()) {
    std::cerr << "Error: Failed to start agent" << std::endl;
    return false;
  }

  if (config_.verbose) {
    std::cout << "MMAgent initialized: " << agent_->get_name() << std::endl;
    std::cout << "Workflows loaded: " << agent_->get_workflows().size() << std::endl;
  }

  return true;
}

void InteractiveRunner::run() {
  std::cout << "\n=== AgentFlow Interactive Mode ===" << std::endl;
  std::cout << "Package: " << package_path_ << std::endl;
  std::cout << "MMAgent: " << agent_->get_name() << std::endl;
  std::cout << "Type 'help' for available commands.\n" << std::endl;

  print_help();

  std::string line;
  while (std::getline(std::cin, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    if (!handle_command(line)) {
      break;
    }

    std::cout << std::endl;
  }
}

std::string InteractiveRunner::execute_workflow(const std::string& workflow_id,
                                                 const std::string& input) {
  // 方案1：使用框架封装的高层次接口
  // 这是最简洁的方式，但不经过消息队列
  if (config_.verbose) {
    std::cout << "[DEBUG] Using high-level execute_workflow interface" << std::endl;
  }

  // 构造输入数据
  DataMap inputs;
  if (!input.empty()) {
    inputs["input"] = input;
  }

  // 直接调用 Runtime 的高层次封装接口
  bool success = runtime_->execute_workflow(agent_->get_id(), workflow_id, inputs);

  if (!success) {
    return "[ERROR] Failed to execute workflow: " + workflow_id;
  }

  // 等待执行完成
  if (!wait_for_completion(config_.timeout_ms)) {
    return "[ERROR] Execution timeout";
  }

  // 收集结果 (从 MMDataBus)
  auto& databus = runtime_->get_databus(agent_->get_id());

  std::ostringstream result;
  // 尝试获取常见的结果键
  std::vector<std::string> result_keys = {"output", "result", "response"};
  for (const auto& key : result_keys) {
    DataVariant value;
    if (databus.get(key, value)) {
      if (std::holds_alternative<std::string>(value)) {
        result << std::get<std::string>(value) << "\n";
      }
    }
  }

  // 如果 MMDataBus 中没有结果，返回成功状态
  if (result.str().empty()) {
    result << "[OK] MMWorkflow " << workflow_id << " executed successfully\n";
  }

  return result.str();
}

std::string InteractiveRunner::list_workflows() const {
  std::ostringstream oss;
  oss << "=== Workflows ===\n";

  auto workflows = agent_->get_workflows();
  for (const auto& wf : workflows) {
    oss << "  [" << wf->get_id() << "] " << wf->get_name()
        << " (" << wf->get_nodes().size() << " nodes)\n";

    // Show nested workflow references
    for (const auto& node : wf->get_nodes()) {
      if (node->get_type() == "workflow") {
        oss << "    └─> workflow_ref: " << node->get_config().workflow_ref << "\n";
      }
    }
  }

  return oss.str();
}

std::string InteractiveRunner::get_status() const {
  std::ostringstream oss;
  oss << "=== Status ===\n";
  oss << "  MMAgent: " << agent_->get_name() << "\n";
  oss << "  MMAgent ID: " << agent_->get_id() << "\n";
  oss << "  State: " << static_cast<int>(agent_->get_state()) << "\n";
  oss << "  Workflows: " << agent_->get_workflows().size() << "\n";
  oss << "  Runtime running: " << (runtime_ && runtime_->is_running() ? "yes" : "no") << "\n";
  oss << "  Active agents: " << (runtime_ ? runtime_->get_active_agents() : 0) << "\n";
  oss << "  Active workflows: " << (runtime_ ? runtime_->get_active_workflows() : 0) << "\n";
  oss << "  Running nodes: " << (runtime_ ? runtime_->get_running_nodes() : 0) << "\n";
  return oss.str();
}

void InteractiveRunner::submit_user_request(const std::string& workflow_id,
                                             const std::string& input) {
  // Construct message
  framework::MMAgentMsgQue::Message message;
  message.message_id = generate_message_id();
  message.type = framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
  message.metadata["message_type"] = "intent_request";
  message.sender_id = "interactive_user";
  message.receiver_id = agent_->get_id();
  message.content = input;
  message.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

  // Set metadata for routing and task creation
  message.metadata["user_id"] = "interactive_user";
  message.metadata["client_id"] = "cli";
  message.metadata["workflow_id"] = workflow_id;

  // Route message through framework
  agent_->route_request_message(message);

  if (config_.verbose) {
    std::cout << "[DEBUG] Submitted request: message_id=" << message.message_id
              << ", workflow_id=" << workflow_id << std::endl;
  }
}

void InteractiveRunner::process_pending_tasks() {
  // Process all sessions until idle
  int tasks_generated = agent_->process_all_sessions_until_idle();

  if (config_.verbose) {
    std::cout << "[DEBUG] Tasks generated by coordinator: " << tasks_generated << std::endl;
  }

  // Get generated tasks and schedule them through runtime
  std::shared_ptr<framework::MMAgentTask> task;
  while (agent_->pop_generated_task(task)) {
    if (!task) {
      continue;
    }

    // Create schedule request
    runtime::master::TaskScheduleRequest request;
    request.task_id = task->get_task_id();
    request.workflow_id = task->get_workflow_id();
    request.workflow_file = task->get_workflow_file();
    request.agent_object_id = agent_->get_id();
    request.agent_instance_id = agent_->get_id();

    // Schedule through runtime
    auto result = runtime_->schedule_task(request);

    if (config_.verbose) {
      std::cout << "[DEBUG] Scheduled task: " << task->get_task_id()
                << " -> workflow_instance=" << result.workflow_instance_id
                << ", worker=" << result.worker_id
                << ", success=" << result.scheduled << std::endl;
    }

    if (!result.scheduled) {
      std::cerr << "[WARN] Failed to schedule task: " << task->get_task_id() << std::endl;
    }
  }
}

bool InteractiveRunner::wait_for_completion(int timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  const int poll_interval_ms = 50;

  while (true) {
    // Check if there are any running nodes
    int running_nodes = runtime_->get_running_nodes();

    // Check if agent has pending messages
    auto session = agent_->get_default_session();
    int pending_receive = session ? session->get_receive_queue_size() : 0;
    int pending_send = session ? session->get_send_queue_size() : 0;

    // If nothing is running and no pending messages, we're done
    if (running_nodes == 0 && pending_receive == 0 && pending_send == 0) {
      return true;
    }

    // Check timeout
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (elapsed >= timeout_ms) {
      return false;
    }

    // Sleep a bit before polling again
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
  }
}

std::string InteractiveRunner::collect_results() {
  auto session = agent_->get_default_session();
  if (!session) {
    return "";
  }

  std::ostringstream result;
  framework::MMAgentMsgQue::Message response;

  while (session->dequeue_send_message(response)) {
    if (!response.content.empty()) {
      result << response.content;
      if (response.content.back() != '\n') {
        result << "\n";
      }
    }

    // Output any metadata
    for (const auto& [key, value] : response.metadata) {
      if (config_.verbose) {
        result << "  [" << key << "] " << value << "\n";
      }
    }
  }

  return result.str();
}

std::string InteractiveRunner::generate_message_id() {
  return "msg_" + std::to_string(++message_counter_) + "_" +
         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}

void InteractiveRunner::print_help() const {
  std::cout << "Available commands:\n"
            << "  execute <workflow_id> [--input <text>]  Execute a workflow\n"
            << "  list                                   List available workflows\n"
            << "  status                                 Show agent status\n"
            << "  help                                   Show this help\n"
            << "  quit / exit                            Exit interactive mode\n"
            << "\n"
            << "Example:\n"
            << "  execute my-workflow --input \"hello world\"\n"
            << std::endl;
}

bool InteractiveRunner::handle_command(const std::string& command_line) {
  auto args = split_args(command_line);
  if (args.empty()) {
    return true;
  }

  const std::string& cmd = args[0];

  if (cmd == "quit" || cmd == "exit") {
    std::cout << "Goodbye!" << std::endl;
    return false;
  }

  if (cmd == "help") {
    print_help();
    return true;
  }

  if (cmd == "list") {
    std::cout << list_workflows() << std::endl;
    return true;
  }

  if (cmd == "status") {
    std::cout << get_status() << std::endl;
    return true;
  }

  if (cmd == "execute") {
    if (args.size() < 2) {
      std::cerr << "Error: execute requires a workflow_id" << std::endl;
      std::cout << "Usage: execute <workflow_id> [--input <text>]" << std::endl;
      return true;
    }

    std::string workflow_id = args[1];
    std::string input;

    // Parse --input argument
    for (size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--input" && i + 1 < args.size()) {
        input = args[i + 1];
        ++i;
      }
    }

    std::cout << "Executing workflow: " << workflow_id << std::endl;
    if (config_.verbose && !input.empty()) {
      std::cout << "Input: " << input << std::endl;
    }

    // Verify workflow exists
    auto workflow = agent_->get_workflow(workflow_id);
    if (!workflow) {
      std::cerr << "Error: MMWorkflow not found: " << workflow_id << std::endl;
      std::cout << "Use 'list' to see available workflows." << std::endl;
      return true;
    }

    // Execute through message-driven pipeline
    auto result = execute_workflow(workflow_id, input);
    std::cout << "--- Output ---\n" << result << std::endl;
    return true;
  }

  std::cerr << "Unknown command: " << cmd << std::endl;
  std::cout << "Type 'help' for available commands." << std::endl;
  return true;
}

}  // namespace agentflow
