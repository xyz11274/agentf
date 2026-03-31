#include "workflow/node.h"

#include "agent/data_variant_utils.h"
#include "runtime/databus/node_databus_adapter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <thread>
#include <type_traits>

#include "runtime/worker.h"
#include "utils/logger.h"
#include "utils/thread_pool.h"

namespace agentflow {
namespace framework {

namespace {
bool parse_flat_json_object(const std::string& json, DataMap& out) {
  return parse_json_object_to_datamap(json, out);
}
}  // namespace

MMNode::MMNode(const Config& config) : config_(config), state_(NodeState::WAITING), retry_count_(0) {
  // Get reference to global code loader
  code_loader_ = &runtime::global_code_loader();

  if(config_.enable_hot_reload && !config_.command.empty()) {
    hot_reload_enabled_ = true;
  }
}

MMNode::~MMNode() {
  stop_runtime();
  if(hot_reload_enabled_) {
    cleanup_hot_reload();
  }
}

std::shared_ptr<MMNode> MMNode::clone(bool inherit_databus) const {
  // Create a new node with the same configuration
  auto cloned = std::make_shared<MMNode>(config_);

  // Fresh state - start in WAITING state (will be set to READY by workflow execution)
  cloned->state_ = NodeState::WAITING;
  cloned->retry_count_ = 0;
  cloned->inputs_.clear();
  cloned->outputs_.clear();
  cloned->last_error_.clear();

  // Optionally inherit databus references for session isolation
  if(inherit_databus) {
    cloned->databus_ = databus_;
    cloned->databus_client_ = databus_client_;
    cloned->plugin_uds_path_ = plugin_uds_path_;
  }

  // Note: code_loader_ is already set in constructor via global_code_loader()
  // Note: Runtime state (threads, pools) is not cloned - fresh node doesn't need runtime

  return cloned;
}

bool MMNode::start_runtime() {
  return start_runtime(RuntimeConfig{});
}

bool MMNode::start_runtime(const RuntimeConfig& config) {
  bool expected = false;
  if(!runtime_running_.compare_exchange_strong(expected, true)) {
    return false;
  }

  runtime_config_ = config;
  executor_pool_  = std::make_unique<basic::ThreadPool>(
      static_cast<size_t>(std::max(1, runtime_config_.executor_threads)));

  message_thread_ = std::thread(&MMNode::message_dispatch_loop, this);
  return true;
}

void MMNode::stop_runtime() {
  bool expected = true;
  if(!runtime_running_.compare_exchange_strong(expected, false)) {
    return;
  }

  message_cv_.notify_all();

  if(message_thread_.joinable()) {
    message_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    high_priority_messages_.clear();
    normal_priority_messages_.clear();
  }

  executor_pool_.reset();
}

bool MMNode::enqueue_message(const Message& message) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  const size_t queued_size = high_priority_messages_.size() + normal_priority_messages_.size();
  if(queued_size >= runtime_config_.max_queue_size) {
    return false;
  }

  if(message.priority == MessagePriority::HIGH) {
    high_priority_messages_.push_back(message);
  } else {
    normal_priority_messages_.push_back(message);
  }
  message_cv_.notify_all();
  return true;
}

bool MMNode::enqueue_event(const NodeEvent& event) {
  (void)event;
  return true;
}

void MMNode::register_event_handler(const std::string& event_type, NodeEventHandler handler) {
  (void)event_type;
  (void)handler;
}

void MMNode::bind_context_workflow(const std::string& workflow_file) {
  config_.node_context_workflow = workflow_file;
}

void MMNode::message_dispatch_loop() {
  while(runtime_running_) {
    Message message;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      message_cv_.wait(lock, [this]() {
        return !high_priority_messages_.empty() || !normal_priority_messages_.empty()
               || !runtime_running_;
      });
      if(!runtime_running_) {
        break;
      }

      if(!high_priority_messages_.empty()) {
        message = high_priority_messages_.front();
        high_priority_messages_.pop_front();
      } else if(!normal_priority_messages_.empty()) {
        message = normal_priority_messages_.front();
        normal_priority_messages_.pop_front();
      } else {
        continue;
      }
    }

    if(executor_pool_) {
      executor_pool_->submit([this, message]() { process_message(message); });
    } else {
      process_message(message);
    }
  }
}

void MMNode::event_dispatch_loop() {
  // Node event dispatch has been retired in favor of worker control/business queues.
}

void MMNode::process_message(const Message& message) {
  std::lock_guard<std::mutex> exec_lock(execution_mutex_);
  set_inputs(message.inputs);
  if(get_state() == NodeState::WAITING) {
    initialize();
  } else if(get_state() != NodeState::READY && get_state() != NodeState::RETRYING) {
    set_state(NodeState::READY);
  }

  const bool success = execute();

  NodeEvent completion_event;
  completion_event.type = success ? "node_message_success" : "node_message_failed";
  completion_event.data["message_id"] = message.message_id;
  completion_event.data["node_id"] = get_id();
  enqueue_event(completion_event);
}

bool MMNode::initialize() {
  // Initialize node control plane
  retry_count_ = 0;
  last_error_.clear();
  state_ = NodeState::READY;

  Logger::instance().info_f("[NODE_INIT] NodeId=%s, NodeName=%s, HotReload=%s", config_.id.c_str(),
                            config_.name.c_str(), hot_reload_enabled_ ? "ENABLED" : "DISABLED");

  if(hot_reload_enabled_) {
    setup_hot_reload();
  }

  return true;
}

bool MMNode::execute() {
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
    case ExecutionMode::GO_SCRIPT:
      mode_str = "GO_SCRIPT";
      break;
    case ExecutionMode::PYTHON_SCRIPT:
      mode_str = "PYTHON_SCRIPT";
      break;
    case ExecutionMode::JAVA_JAR:
      mode_str = "JAVA_JAR";
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
    std::string value_str = data_variant_to_string(pair.second);
    Logger::instance().debug_f("[NODE_EXECUTE_START] NodeId=%s, Input[%s]=%s", config_.id.c_str(),
                               pair.first.c_str(), value_str.c_str());
  }

  // Execute node task
  if(state_ != NodeState::READY && state_ != NodeState::RETRYING) {
    Logger::instance().error_f(
        "[NODE_EXECUTE_ERROR] MMNode %s not in executable state, current state=%d",
        config_.name.c_str(), static_cast<int>(state_));
    return false;
  }

  state_ = NodeState::EXECUTING;

  // Check and reload code if hot reload is enabled
  if(hot_reload_enabled_) {
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
    case ExecutionMode::GO_SCRIPT:
    case ExecutionMode::JAVA_JAR:
    case ExecutionMode::SO_LIBRARY:
      result = execute_with_worker_engine();
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
    std::string value_str = data_variant_to_string(pair.second);
    Logger::instance().debug_f("[NODE_EXECUTE_END] NodeId=%s, Output[%s]=%s", config_.id.c_str(),
                               pair.first.c_str(), value_str.c_str());
  }

  return result;
}

bool MMNode::cleanup() {
  // Clean up node resources
  Logger::instance().info_f("MMNode cleanup: %s", config_.name.c_str());
  state_ = NodeState::WAITING;
  inputs_.clear();
  outputs_.clear();
  last_error_.clear();
  if(hot_reload_enabled_) {
    cleanup_hot_reload();
  }
  return true;
}

bool MMNode::execute_cpp_function() {
  auto start_time = std::chrono::steady_clock::now();

  Logger::instance().info_f("[CPP_FUNC_START] NodeId=%s, NodeName=%s", config_.id.c_str(),
                            config_.name.c_str());

  bool success = false;

  if(config_.cpp_function) {
    // Use the registered C++ function callback
    try {
      success = config_.cpp_function(inputs_, outputs_);
    } catch(const std::exception& e) {
      last_error_ = std::string("C++ function threw exception: ") + e.what();
      Logger::instance().error_f("[CPP_FUNC_ERROR] NodeId=%s, Error=%s", config_.id.c_str(),
                                 e.what());
      return false;
    }
  } else {
    // No callback registered — fallback to CodeLoader if function_name is set
    if(!config_.function_name.empty() && code_loader_) {
      auto result = code_loader_->execute(config_.command);
      success     = result.success;
      if(success && !result.stdout.empty()) {
        outputs_["result"] = result.stdout;
      }
      if(!result.error_message.empty()) {
        last_error_ = result.error_message;
      }
    } else {
      last_error_ = "No cpp_function callback or function_name configured";
      Logger::instance().warn_f("[CPP_FUNC_WARN] NodeId=%s, %s", config_.id.c_str(),
                                last_error_.c_str());
      return false;
    }
  }

  auto end_time = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  if(success) {
    Logger::instance().info_f("[CPP_FUNC_SUCCESS] NodeId=%s, NodeName=%s, Duration=%ldms",
                              config_.id.c_str(), config_.name.c_str(), duration);
  } else {
    Logger::instance().error_f("[CPP_FUNC_FAILED] NodeId=%s, Duration=%ldms, Error=%s",
                               config_.id.c_str(), duration, last_error_.c_str());
  }

  return success;
}

bool MMNode::execute_shell_command() {
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

bool MMNode::execute_with_timeout(int timeout_ms, std::function<bool()> executor) {
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

bool MMNode::execute_with_worker_engine() {
  runtime::MMWorkerEngine::ExecutionRequest request;
  request.command       = config_.command;
  request.function_name = config_.function_name;
  request.main_class    = config_.main_class;
  request.working_dir   = config_.working_dir;
  request.args          = config_.args;
  request.timeout_ms    = config_.timeout_ms > 0 ? config_.timeout_ms : 30000;
  request.inputs        = inputs_;
  request.enable_hot_reload = hot_reload_enabled_;
  request.hot_reload_config = config_.hot_reload_config;

  // Pass DataBus and session_id for V2/V3 signature support
  request.databus = databus_;
  if (databus_client_) {
    request.session_id = databus_client_->session_id();
    // Pass the full client for V3 async operations
    request.databus_client = databus_client_.get();
  }

  // Pass plugin UDS path for external plugin communication
  request.plugin_uds_path = plugin_uds_path_;

  // Pass DataBus UDS path for workflow-level DataBus access
  request.databus_uds_path = databus_uds_path_;

  switch(config_.execution_mode) {
    case ExecutionMode::SHELL_SCRIPT:
      request.mode = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
      break;
    case ExecutionMode::EXECUTABLE:
      request.mode = runtime::MMWorkerEngine::ExecutionMode::EXECUTABLE;
      break;
    case ExecutionMode::GO_SCRIPT:
      request.mode = runtime::MMWorkerEngine::ExecutionMode::GO_SCRIPT;
      break;
    case ExecutionMode::PYTHON_SCRIPT:
      request.mode = runtime::MMWorkerEngine::ExecutionMode::PYTHON_SCRIPT;
      break;
    case ExecutionMode::JAVA_JAR:
      request.mode = runtime::MMWorkerEngine::ExecutionMode::JAVA_JAR;
      break;
    case ExecutionMode::SO_LIBRARY:
      request.mode = runtime::MMWorkerEngine::ExecutionMode::SO_LIBRARY;
      break;
    case ExecutionMode::CPP_FUNCTION:
    default:
      request.mode = runtime::MMWorkerEngine::ExecutionMode::CPP_FUNCTION;
      break;
  }

  auto response = runtime::MMWorkerEngine::execute_code(request);
  auto apply_worker_outputs = [this](const runtime::MMWorkerEngine::ExecutionResponse& worker_response) {
    outputs_ = worker_response.outputs;
    if(config_.json_output && !worker_response.stdout_content.empty()) {
      DataMap parsed;
      if(parse_flat_json_object(worker_response.stdout_content, parsed)) {
        for(auto& kv : parsed) {
          outputs_[kv.first] = std::move(kv.second);
        }
      }
    }
  };
  if(response.success) {
    apply_worker_outputs(response);
    return true;
  }
  if(!response.outputs.empty()) {
    apply_worker_outputs(response);
  }
  last_error_ = response.error_message.empty() ? "Worker execution failed" : response.error_message;
  return false;
}

// ============================================================================
// Hot Reload Implementation
// ============================================================================

bool MMNode::enable_code_reload() {
  if(config_.command.empty()) {
    return false;
  }
  hot_reload_enabled_ = true;
  setup_hot_reload();

  Logger::instance().info_f("[NODE_HOT_RELOAD] Enabled for node: %s", config_.name.c_str());
  return true;
}

bool MMNode::disable_code_reload() {
  hot_reload_enabled_ = false;
  cleanup_hot_reload();

  Logger::instance().info_f("[NODE_HOT_RELOAD] Disabled for node: %s", config_.name.c_str());
  return true;
}

bool MMNode::reload_code() {
  if(config_.command.empty()) {
    return false;
  }

  if(runtime::MMWorkerEngine::check_and_reload_code(config_.command, config_.hot_reload_config)) {
    auto current_version = runtime::MMWorkerEngine::get_code_version(config_.command);
    current_version_id_  = current_version ? current_version->version_id : current_version_id_;
    Logger::instance().info_f("[NODE_HOT_RELOAD] Reload success: %s, Version=%s",
                              config_.name.c_str(), current_version_id_.c_str());
    return true;
  }

  Logger::instance().error_f("[NODE_HOT_RELOAD] Reload failed: %s", config_.name.c_str());
  return false;
}

bool MMNode::check_and_reload_code() {
  if(!hot_reload_enabled_ || config_.command.empty()) {
    return false;
  }

  if(runtime::MMWorkerEngine::check_and_reload_code(config_.command, config_.hot_reload_config)) {
    auto current_version = runtime::MMWorkerEngine::get_code_version(config_.command);
    current_version_id_  = current_version ? current_version->version_id : current_version_id_;
    Logger::instance().info_f("[NODE_HOT_RELOAD] File change detected and reloaded: %s, Version=%s",
                              config_.name.c_str(), current_version_id_.c_str());
    return true;
  }

  return false;
}

std::shared_ptr<runtime::ExecutableVersion> MMNode::get_code_version() const {
  if(!hot_reload_enabled_ || config_.command.empty()) {
    return nullptr;
  }

  return runtime::MMWorkerEngine::get_code_version(config_.command);
}

bool MMNode::rollback_to_version(const std::string& version_id) {
  if(!hot_reload_enabled_ || config_.command.empty()) {
    return false;
  }

  bool result = runtime::MMWorkerEngine::rollback_code_version(config_.command, version_id);

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

void MMNode::setup_hot_reload() {
  if(config_.command.empty()) {
    return;
  }

  runtime::MMWorkerEngine::check_and_reload_code(config_.command, config_.hot_reload_config);
  auto version = runtime::MMWorkerEngine::get_code_version(config_.command);
  if(version) {
    current_version_id_ = version->version_id;
    Logger::instance().info_f("[NODE_HOT_RELOAD] Setup complete: %s, InitialVersion=%s",
                              config_.name.c_str(), current_version_id_.c_str());
  }
}

void MMNode::cleanup_hot_reload() {
  if(config_.command.empty()) {
    return;
  }

  runtime::MMWorkerEngine::stop_hot_reload(config_.command);

  Logger::instance().info_f("[NODE_HOT_RELOAD] Cleanup complete: %s", config_.name.c_str());
}

// ============================================================================
// SO Library Execution
// ============================================================================

bool MMNode::execute_so_library() {
  auto start_time = std::chrono::steady_clock::now();

  Logger::instance().info_f("[SO_LIB_START] NodeId=%s, NodeName=%s, Lib=%s, Function=%s",
                            config_.id.c_str(), config_.name.c_str(), config_.command.c_str(),
                            config_.function_name.c_str());

  if(code_loader_) {
    runtime::ExecutionConfig exec_config;
    exec_config.inputs      = inputs_;
    exec_config.working_dir = config_.working_dir;
    exec_config.timeout_ms  = config_.timeout_ms > 0 ? config_.timeout_ms : 30000;

    // Pass DataBus client for V3 signature support (preferred)
    if(databus_client_) {
      exec_config.databus_client = databus_client_.get();
    }
    // Fall back to DataBus for V2 signature support
    else if(databus_) {
      exec_config.databus = new runtime::NodeDataBusAdapter(databus_);
    }

    auto result = code_loader_->load_and_execute_so(
        config_.command, config_.function_name.empty() ? "execute" : config_.function_name,
        exec_config);

    // Clean up adapter if created (only for V2 fallback)
    if(exec_config.databus) {
      delete static_cast<runtime::NodeDataBusAdapter*>(exec_config.databus);
      exec_config.databus = nullptr;
    }

    if(result.success) {
      // Parse outputs
      std::istringstream iss(result.stdout);
      std::string        line;
      while(std::getline(iss, line)) {
        size_t pos = line.find('=');
        if(pos != std::string::npos) {
          std::string key   = line.substr(0, pos);
          std::string value = line.substr(pos + 1);

          // Convert special keys to their proper types
          if(key == "__async_pending__") {
            // Convert string "true"/"1" to bool
            outputs_[key] = (value == "true" || value == "1");
          } else {
            outputs_[key] = value;
          }
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

runtime::ExecutionResult MMNode::execute_with_code_loader() {
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

bool MMNode::execute_script(runtime::ExecutableType type) {
  auto start_time = std::chrono::steady_clock::now();

  std::string type_str = runtime::CodeLoader::executable_type_to_string(type);
  Logger::instance().info_f("[SCRIPT_EXEC_START] NodeId=%s, NodeName=%s, Type=%s, Script=%s",
                            config_.id.c_str(), config_.name.c_str(), type_str.c_str(),
                            config_.command.c_str());

  if(code_loader_) {
    runtime::ExecutionConfig exec_config;
    exec_config.inputs      = inputs_;
    exec_config.working_dir = config_.working_dir;
    exec_config.timeout_ms  = config_.timeout_ms > 0 ? config_.timeout_ms : 30000;
    auto to_env_suffix = [](const std::string& key) {
      std::string out;
      out.reserve(key.size());
      for(char ch : key) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if(std::isalnum(uch)) {
          out.push_back(static_cast<char>(std::toupper(uch)));
        } else {
          out.push_back('_');
        }
      }
      return out;
    };
    for(const auto& pair : inputs_) {
      std::string env_key = "AF_" + to_env_suffix(pair.first);
      exec_config.env_vars.push_back(env_key + "=" + data_variant_to_string(pair.second));
    }
    if(config_.json_output) {
      exec_config.env_vars.push_back("AF_OUTPUT_FORMAT=json");
    }

    runtime::ExecutionResult result;

    switch(type) {
      case runtime::ExecutableType::SHELL_SCRIPT:
        result = code_loader_->execute_shell_script(config_.command, config_.args, exec_config);
        break;
      case runtime::ExecutableType::EXECUTABLE:
        result = code_loader_->execute_shell_script(config_.command, config_.args, exec_config);
        break;
      case runtime::ExecutableType::PYTHON_SCRIPT:
        result = code_loader_->execute_python_script(config_.command, config_.args, exec_config);
        break;
      case runtime::ExecutableType::GO_SCRIPT:
        result = code_loader_->execute_go_script(config_.command, config_.args, exec_config);
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
      // When json_output is enabled, attempt to parse stdout as a JSON object and
      // populate outputs_ with individual keys. Falls back to raw stdout on failure.
      bool json_parsed = false;
      if(config_.json_output && !result.stdout.empty()) {
        DataMap parsed;
        if(parse_flat_json_object(result.stdout, parsed)) {
          for(auto& kv : parsed) {
            outputs_[kv.first] = std::move(kv.second);
          }
          json_parsed = true;
        }
      }
      if(!json_parsed) {
        outputs_["stdout"] = result.stdout;
      }
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

bool MMNode::execute_jar_file() {
  return execute_script(runtime::ExecutableType::JAR_FILE);
}

}  // namespace framework
}  // namespace agentflow
