#include "agentflow/framework/node.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

#include "agentflow/basic/logger.h"
#include "agentflow/basic/thread_pool.h"

namespace agentflow {
namespace framework {

Node::Node(const Config& config) : config_(config), state_(NodeState::WAITING), retry_count_(0) {
  // Get reference to global code loader
  code_loader_ = &runtime::global_code_loader();

  // Setup hot reload if enabled
  if(config_.enable_hot_reload && !config_.command.empty()) {
    hot_reload_enabled_ = true;
  }
}

bool Node::initialize() {
  // Initialize node control plane
  retry_count_ = 0;
  last_error_.clear();
  state_ = NodeState::READY;

  Logger::instance().info_f("[NODE_INIT] NodeId=%s, NodeName=%s, HotReload=%s", config_.id.c_str(),
                            config_.name.c_str(),
                            config_.enable_hot_reload ? "ENABLED" : "DISABLED");

  // Setup hot reload if enabled
  if(config_.enable_hot_reload && hot_reload_enabled_) {
    setup_hot_reload();
  }

  return true;
}

bool Node::execute() {
  // [DEBUG] Log node execution start
  auto start_time = std::chrono::steady_clock::now();

  std::string mode_str = "UNKNOWN";
  switch(config_.execution_mode) {
    case ExecutionMode::CPP_FUNCTION:
      mode_str = "CPP_FUNCTION";
      break;
    case ExecutionMode::SHELL_SCRIPT:
      mode_str = "SHELL_SCRIPT";
      break;
    case ExecutionMode::EXECUTABLE:
      mode_str = "EXECUTABLE";
      break;
    case ExecutionMode::PYTHON_SCRIPT:
      mode_str = "PYTHON_SCRIPT";
      break;
    case ExecutionMode::SO_LIBRARY:
      mode_str = "SO_LIBRARY";
      break;
  }

  Logger::instance().info_f(
      "[NODE_EXECUTE_START] NodeId=%s, NodeName=%s, Mode=%s, State=%d, RetryCount=%d",
      config_.id.c_str(), config_.name.c_str(), mode_str.c_str(), static_cast<int>(state_),
      retry_count_);

  // Log input data
  Logger::instance().debug_f("[NODE_EXECUTE_START] NodeId=%s, Inputs: %zu entries",
                             config_.id.c_str(), inputs_.size());
  for(const auto& pair : inputs_) {
    std::string value_str = std::visit(
        [](auto&& arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) {
            return arg;
          } else if constexpr(std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
          } else {
            return std::to_string(arg);
          }
        },
        pair.second);
    Logger::instance().debug_f("[NODE_EXECUTE_START] NodeId=%s, Input[%s]=%s", config_.id.c_str(),
                               pair.first.c_str(), value_str.c_str());
  }

  // Execute node task
  if(state_ != NodeState::READY && state_ != NodeState::RETRYING) {
    Logger::instance().error_f(
        "[NODE_EXECUTE_ERROR] Node %s not in executable state, current state=%d",
        config_.name.c_str(), static_cast<int>(state_));
    return false;
  }

  state_ = NodeState::EXECUTING;

  // Check and reload code if hot reload is enabled
  if(hot_reload_enabled_ && config_.enable_hot_reload) {
    check_and_reload_code();
  }

  // Execute based on execution mode
  bool result = false;
  switch(config_.execution_mode) {
    case ExecutionMode::CPP_FUNCTION:
      result = execute_cpp_function();
      break;
    case ExecutionMode::SHELL_SCRIPT:
    case ExecutionMode::PYTHON_SCRIPT:
    case ExecutionMode::EXECUTABLE:
      result = execute_shell_command();
      break;
    case ExecutionMode::SO_LIBRARY:
      result = execute_so_library();
      break;
  }

  // Calculate execution time
  auto end_time = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  if(result) {
    state_       = NodeState::SUCCESS;
    retry_count_ = 0;
    Logger::instance().info_f("[NODE_EXECUTE_SUCCESS] NodeId=%s, NodeName=%s, Duration=%ldms",
                              config_.id.c_str(), config_.name.c_str(), duration);
  } else {
    state_ = NodeState::FAILED;
    Logger::instance().error_f(
        "[NODE_EXECUTE_FAILED] NodeId=%s, NodeName=%s, Duration=%ldms, Error=%s",
        config_.id.c_str(), config_.name.c_str(), duration, last_error_.c_str());
  }

  // Log output data
  Logger::instance().debug_f("[NODE_EXECUTE_END] NodeId=%s, Outputs: %zu entries",
                             config_.id.c_str(), outputs_.size());
  for(const auto& pair : outputs_) {
    std::string value_str = std::visit(
        [](auto&& arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) {
            return arg;
          } else if constexpr(std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
          } else {
            return std::to_string(arg);
          }
        },
        pair.second);
    Logger::instance().debug_f("[NODE_EXECUTE_END] NodeId=%s, Output[%s]=%s", config_.id.c_str(),
                               pair.first.c_str(), value_str.c_str());
  }

  return result;
}

bool Node::cleanup() {
  // Clean up node resources
  Logger::instance().info_f("Node cleanup: %s", config_.name.c_str());
  state_ = NodeState::WAITING;
  inputs_.clear();
  outputs_.clear();
  last_error_.clear();
  return true;
}

bool Node::execute_cpp_function() {
  auto start_time = std::chrono::steady_clock::now();

  // [DEBUG] Log C++ function execution start
  Logger::instance().info_f("[CPP_FUNC_START] NodeId=%s, NodeName=%s", config_.id.c_str(),
                            config_.name.c_str());

  // For C++ function execution, we simulate a simple operation
  // In real implementation, this would call a registered callback or plugin function
  Logger::instance().debug_f("[CPP_FUNC_EXEC] NodeId=%s, Executing C++ function",
                             config_.id.c_str());

  // Simulate some work
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Set output (simulated)
  outputs_["result"]            = "success";
  outputs_["execution_time_ms"] = "100";

  auto end_time = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  // [DEBUG] Log C++ function execution end
  Logger::instance().info_f("[CPP_FUNC_SUCCESS] NodeId=%s, NodeName=%s, Duration=%ldms",
                            config_.id.c_str(), config_.name.c_str(), duration);

  return true;
}

bool Node::execute_shell_command() {
  auto start_time = std::chrono::steady_clock::now();

  // [DEBUG] Log shell command execution start
  Logger::instance().info_f("[SHELL_CMD_START] NodeId=%s, NodeName=%s, Command=%s",
                            config_.id.c_str(), config_.name.c_str(), config_.command.c_str());

  if(config_.command.empty()) {
    last_error_ = "No command specified for execution";
    Logger::instance().error_f("[SHELL_CMD_ERROR] NodeId=%s, Error=%s", config_.id.c_str(),
                               last_error_.c_str());
    return false;
  }

  std::string full_command = config_.command;

  // Note: For simplicity, we're not implementing timeout via system timeout command
  // as it may not be available on all systems. In production, use proper
  // process management with kill signals.

  Logger::instance().debug_f("[SHELL_CMD_EXEC] NodeId=%s, Executing: %s", config_.id.c_str(),
                             full_command.c_str());

  // Execute command
  FILE* pipe = popen(full_command.c_str(), "r");
  if(!pipe) {
    last_error_ = "Failed to execute command: " + full_command;
    Logger::instance().error_f("[SHELL_CMD_ERROR] NodeId=%s, Error=%s", config_.id.c_str(),
                               last_error_.c_str());
    return false;
  }

  // Read output
  char        buffer[128];
  std::string output;
  while(fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }

  int status    = pclose(pipe);
  int exit_code = WEXITSTATUS(status);

  auto end_time = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  if(status == 0 || exit_code == 0) {
    // Command succeeded
    outputs_["stdout"]    = output;
    outputs_["exit_code"] = "0";

    // [DEBUG] Log shell command execution success
    Logger::instance().info_f(
        "[SHELL_CMD_SUCCESS] NodeId=%s, NodeName=%s, Duration=%ldms, ExitCode=%d",
        config_.id.c_str(), config_.name.c_str(), duration, exit_code);

    // Log output (truncated if too long)
    std::string truncated_output = output;
    if(truncated_output.length() > 200) {
      truncated_output = truncated_output.substr(0, 200) + "...";
    }
    Logger::instance().debug_f("[SHELL_CMD_OUTPUT] NodeId=%s, Output=%s", config_.id.c_str(),
                               truncated_output.c_str());

    return true;
  } else {
    // Command failed
    last_error_           = "Command failed with exit code: " + std::to_string(exit_code);
    outputs_["stdout"]    = output;
    outputs_["exit_code"] = std::to_string(exit_code);

    // [DEBUG] Log shell command execution failure
    Logger::instance().error_f(
        "[SHELL_CMD_FAILED] NodeId=%s, NodeName=%s, Duration=%ldms, ExitCode=%d, Error=%s",
        config_.id.c_str(), config_.name.c_str(), duration, exit_code, last_error_.c_str());

    return false;
  }
}

bool Node::execute_with_timeout(int timeout_ms, std::function<bool()> executor) {
  // [DEBUG] Log timeout execution start
  Logger::instance().info_f("[TIMEOUT_EXEC_START] NodeId=%s, NodeName=%s, Timeout=%dms",
                            config_.id.c_str(), config_.name.c_str(), timeout_ms);

  if(timeout_ms <= 0) {
    // No timeout, execute directly
    Logger::instance().debug_f("[TIMEOUT_EXEC_DIRECT] NodeId=%s, No timeout, executing directly",
                               config_.id.c_str());
    bool result = executor();

    Logger::instance().info_f("[TIMEOUT_EXEC_END] NodeId=%s, Result=%s", config_.id.c_str(),
                              result ? "SUCCESS" : "FAILED");
    return result;
  }

  // Execute with timeout using thread pool
  auto start = std::chrono::steady_clock::now();

  // Submit task to global thread pool (capture executor by value to avoid dangling reference)
  auto executor_copy = executor;
  auto future = basic::global_thread_pool().submit([executor_copy]() { return executor_copy(); });

  // Wait for completion or timeout
  bool timed_out = false;
  while(future.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout) {
    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

    if(elapsed >= timeout_ms) {
      timed_out   = true;
      last_error_ = "Execution timed out after " + std::to_string(timeout_ms) + "ms";
      break;
    }
  }

  if(timed_out) {
    Logger::instance().error_f(
        "[TIMEOUT_EXEC_TIMEOUT] NodeId=%s, NodeName=%s, Timeout=%dms reached", config_.id.c_str(),
        config_.name.c_str(), timeout_ms);
    return false;
  }

  // Get the result
  try {
    bool result = future.get();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start).count();

    Logger::instance().info_f(
        "[TIMEOUT_EXEC_END] NodeId=%s, NodeName=%s, Result=%s, Duration=%ldms", config_.id.c_str(),
        config_.name.c_str(), result ? "SUCCESS" : "FAILED", duration);
    return result;
  } catch(const std::exception& e) {
    last_error_ = "Execution exception: " + std::string(e.what());
    Logger::instance().error_f("[TIMEOUT_EXEC_EXCEPTION] NodeId=%s, Exception=%s",
                               config_.id.c_str(), e.what());
    throw;
  } catch(...) {
    last_error_ = "Execution: unknown exception";
    Logger::instance().error_f("[TIMEOUT_EXEC_EXCEPTION] NodeId=%s, Unknown exception",
                               config_.id.c_str());
    throw;
  }
}

// ============================================================================
// Hot Reload Implementation
// ============================================================================

bool Node::enable_code_reload() {
  if(!config_.enable_hot_reload) {
    Logger::instance().warn_f("[NODE_HOT_RELOAD] Hot reload disabled in config for node: %s",
                              config_.name.c_str());
    return false;
  }

  hot_reload_enabled_ = true;
  setup_hot_reload();

  Logger::instance().info_f("[NODE_HOT_RELOAD] Enabled for node: %s", config_.name.c_str());
  return true;
}

bool Node::disable_code_reload() {
  hot_reload_enabled_ = false;
  cleanup_hot_reload();

  Logger::instance().info_f("[NODE_HOT_RELOAD] Disabled for node: %s", config_.name.c_str());
  return true;
}

bool Node::reload_code() {
  if(!hot_reload_enabled_ || config_.command.empty()) {
    return false;
  }

  auto& hot_reload = runtime::global_hot_reload_manager();
  auto  result     = hot_reload.reload(config_.command);

  if(result.success) {
    current_version_id_ = result.version_id;
    Logger::instance().info_f("[NODE_HOT_RELOAD] Reload success: %s, Version=%s",
                              config_.name.c_str(), result.version_id.c_str());
    return true;
  }

  Logger::instance().error_f("[NODE_HOT_RELOAD] Reload failed: %s, Error=%s", config_.name.c_str(),
                             result.message.c_str());
  return false;
}

bool Node::check_and_reload_code() {
  if(!hot_reload_enabled_ || config_.command.empty()) {
    return false;
  }

  auto& hot_reload = runtime::global_hot_reload_manager();

  if(!hot_reload.is_watching(config_.command)) {
    hot_reload.start_watching(config_.command);
  }

  auto current_version = hot_reload.get_current_version(config_.command);
  if(current_version && current_version->version_id != current_version_id_) {
    Logger::instance().info_f("[NODE_HOT_RELOAD] New version detected: %s, Old=%s, New=%s",
                              config_.name.c_str(), current_version_id_.c_str(),
                              current_version->version_id.c_str());

    return reload_code();
  }

  return false;
}

std::shared_ptr<runtime::ExecutableVersion> Node::get_code_version() const {
  if(!hot_reload_enabled_ || config_.command.empty()) {
    return nullptr;
  }

  auto& hot_reload = runtime::global_hot_reload_manager();
  return hot_reload.get_current_version(config_.command);
}

bool Node::rollback_to_version(const std::string& version_id) {
  if(!hot_reload_enabled_ || config_.command.empty()) {
    return false;
  }

  auto& hot_reload = runtime::global_hot_reload_manager();
  bool  result     = hot_reload.rollback_to_version(config_.command, version_id);

  if(result) {
    current_version_id_ = version_id;
    Logger::instance().info_f("[NODE_HOT_RELOAD] Rollback success: %s, Version=%s",
                              config_.name.c_str(), version_id.c_str());
  } else {
    Logger::instance().error_f("[NODE_HOT_RELOAD] Rollback failed: %s, Version=%s",
                               config_.name.c_str(), version_id.c_str());
  }

  return result;
}

void Node::setup_hot_reload() {
  if(config_.command.empty()) {
    return;
  }

  auto& hot_reload = runtime::global_hot_reload_manager();
  hot_reload.set_config(config_.hot_reload_config);

  if(!hot_reload.is_watching(config_.command)) {
    hot_reload.start_watching(config_.command);
  }

  auto version = hot_reload.get_current_version(config_.command);
  if(version) {
    current_version_id_ = version->version_id;
    Logger::instance().info_f("[NODE_HOT_RELOAD] Setup complete: %s, InitialVersion=%s",
                              config_.name.c_str(), current_version_id_.c_str());
  }
}

void Node::cleanup_hot_reload() {
  if(config_.command.empty()) {
    return;
  }

  auto& hot_reload = runtime::global_hot_reload_manager();
  hot_reload.stop_watching(config_.command);

  Logger::instance().info_f("[NODE_HOT_RELOAD] Cleanup complete: %s", config_.name.c_str());
}

// ============================================================================
// SO Library Execution
// ============================================================================

bool Node::execute_so_library() {
  auto start_time = std::chrono::steady_clock::now();

  Logger::instance().info_f("[SO_LIB_START] NodeId=%s, NodeName=%s, Lib=%s, Function=%s",
                            config_.id.c_str(), config_.name.c_str(), config_.command.c_str(),
                            config_.function_name.c_str());

  if(code_loader_) {
    runtime::ExecutionConfig exec_config;
    exec_config.inputs      = inputs_;
    exec_config.working_dir = config_.working_dir;
    exec_config.timeout_ms  = config_.timeout_ms > 0 ? config_.timeout_ms : 30000;

    auto result = code_loader_->load_and_execute_so(
        config_.command, config_.function_name.empty() ? "execute" : config_.function_name,
        exec_config);

    if(result.success) {
      // Parse outputs
      std::istringstream iss(result.stdout);
      std::string        line;
      while(std::getline(iss, line)) {
        size_t pos = line.find('=');
        if(pos != std::string::npos) {
          std::string key   = line.substr(0, pos);
          std::string value = line.substr(pos + 1);
          outputs_[key]     = value;
        }
      }

      auto end_time = std::chrono::steady_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

      Logger::instance().info_f("[SO_LIB_SUCCESS] NodeId=%s, Duration=%ldms", config_.id.c_str(),
                                duration);
      return true;
    } else {
      last_error_   = result.error_message;
      auto end_time = std::chrono::steady_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

      Logger::instance().error_f("[SO_LIB_FAILED] NodeId=%s, Duration=%ldms, Error=%s",
                                 config_.id.c_str(), duration, result.error_message.c_str());
      return false;
    }
  }

  last_error_ = "Code loader not available";
  Logger::instance().error_f("[SO_LIB_FAILED] NodeId=%s, Error=%s", config_.id.c_str(),
                             last_error_.c_str());
  return false;
}

// ============================================================================
// Execute with Code Loader
// ============================================================================

runtime::ExecutionResult Node::execute_with_code_loader() {
  if(!code_loader_) {
    runtime::ExecutionResult result;
    result.success       = false;
    result.error_message = "Code loader not available";
    return result;
  }

  runtime::ExecutionConfig exec_config;
  exec_config.inputs      = inputs_;
  exec_config.working_dir = config_.working_dir;
  exec_config.timeout_ms  = config_.timeout_ms > 0 ? config_.timeout_ms : 30000;

  return code_loader_->execute(config_.command, exec_config);
}

// ============================================================================
// Script Execution (generic)
// ============================================================================

bool Node::execute_script(runtime::ExecutableType type) {
  auto start_time = std::chrono::steady_clock::now();

  std::string type_str = runtime::executable_type_to_string(type);
  Logger::instance().info_f("[SCRIPT_EXEC_START] NodeId=%s, NodeName=%s, Type=%s, Script=%s",
                            config_.id.c_str(), config_.name.c_str(), type_str.c_str(),
                            config_.command.c_str());

  if(code_loader_) {
    runtime::ExecutionConfig exec_config;
    exec_config.inputs      = inputs_;
    exec_config.working_dir = config_.working_dir;
    exec_config.timeout_ms  = config_.timeout_ms > 0 ? config_.timeout_ms : 30000;

    runtime::ExecutionResult result;

    switch(type) {
      case runtime::ExecutableType::PYTHON_SCRIPT:
        result = code_loader_->execute_python_script(config_.command, config_.args, exec_config);
        break;
      case runtime::ExecutableType::NODE_JS_SCRIPT:
        result = code_loader_->execute_nodejs_script(config_.command, config_.args, exec_config);
        break;
      case runtime::ExecutableType::LUA_SCRIPT:
        result = code_loader_->execute_lua_script(config_.command, config_.args, exec_config);
        break;
      case runtime::ExecutableType::RUBY_SCRIPT:
        result = code_loader_->execute_ruby_script(config_.command, config_.args, exec_config);
        break;
      case runtime::ExecutableType::JAR_FILE:
        result = code_loader_->execute_jar(config_.command, config_.main_class, config_.args,
                                           exec_config);
        break;
      default:
        result = code_loader_->execute(config_.command, exec_config);
        break;
    }

    if(result.success) {
      outputs_["stdout"] = result.stdout;
      if(!result.stderr.empty()) {
        outputs_["stderr"] = result.stderr;
      }

      auto end_time = std::chrono::steady_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

      Logger::instance().info_f("[SCRIPT_EXEC_SUCCESS] NodeId=%s, Duration=%ldms",
                                config_.id.c_str(), duration);
      return true;
    } else {
      last_error_        = result.error_message;
      outputs_["stdout"] = result.stdout;
      if(!result.stderr.empty()) {
        outputs_["stderr"] = result.stderr;
      }

      auto end_time = std::chrono::steady_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

      Logger::instance().error_f("[SCRIPT_EXEC_FAILED] NodeId=%s, Duration=%ldms, Error=%s",
                                 config_.id.c_str(), duration, result.error_message.c_str());
      return false;
    }
  }

  last_error_ = "Code loader not available";
  Logger::instance().error_f("[SCRIPT_EXEC_FAILED] NodeId=%s, Error=%s", config_.id.c_str(),
                             last_error_.c_str());
  return false;
}

// ============================================================================
// JAR File Execution
// ============================================================================

bool Node::execute_jar_file() {
  return execute_script(runtime::ExecutableType::JAR_FILE);
}

}  // namespace framework
}  // namespace agentflow
