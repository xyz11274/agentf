#include "agentflow/framework/node.h"
#include "agentflow/basic/logger.h"
#include <thread>
#include <chrono>
#include <cstring>
#include <memory>
#include <functional>

namespace agentflow {
namespace framework {

Node::Node(const Config& config)
    : config_(config), state_(NodeState::WAITING), retry_count_(0) {
}

bool Node::initialize() {
    // Initialize node control plane
    retry_count_ = 0;
    last_error_.clear();
    state_ = NodeState::READY;
    Logger::instance().info_f("Node initialized: %s", config_.name.c_str());
    return true;
}

bool Node::execute() {
    // Execute node task
    if (state_ != NodeState::READY && state_ != NodeState::RETRYING) {
        Logger::instance().error_f("Node %s not in executable state",
                                config_.name.c_str());
        return false;
    }

    state_ = NodeState::EXECUTING;

    // Execute based on execution mode
    bool result = false;
    switch (config_.execution_mode) {
        case ExecutionMode::CPP_FUNCTION:
            result = execute_cpp_function();
            break;
        case ExecutionMode::SHELL_SCRIPT:
        case ExecutionMode::PYTHON_SCRIPT:
        case ExecutionMode::EXECUTABLE:
            result = execute_shell_command();
            break;
        case ExecutionMode::SO_LIBRARY:
            // SO library execution not implemented yet
            Logger::instance().info_f("SO library execution not yet implemented for node: %s",
                                     config_.name.c_str());
            result = false;
            break;
    }

    if (result) {
        state_ = NodeState::SUCCESS;
        retry_count_ = 0;
        Logger::instance().info_f("Node executed successfully: %s",
                                config_.name.c_str());
    } else {
        state_ = NodeState::FAILED;
        Logger::instance().error_f("Node execution failed: %s - %s",
                                 config_.name.c_str(),
                                 last_error_.c_str());
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
    // For C++ function execution, we simulate a simple operation
    // In real implementation, this would call a registered callback or plugin function
    Logger::instance().info_f("Executing C++ function for node: %s",
                            config_.name.c_str());

    // Simulate some work
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Set output (simulated)
    outputs_["result"] = "success";
    outputs_["execution_time_ms"] = "100";

    return true;
}

bool Node::execute_shell_command() {
    Logger::instance().info_f("Executing shell command for node: %s",
                            config_.name.c_str());

    if (config_.command.empty()) {
        last_error_ = "No command specified for execution";
        return false;
    }

    std::string full_command = config_.command;

    // Note: For simplicity, we're not implementing timeout via system timeout command
    // as it may not be available on all systems. In production, use proper
    // process management with kill signals.

    // Execute command
    FILE* pipe = popen(full_command.c_str(), "r");
    if (!pipe) {
        last_error_ = "Failed to execute command: " + full_command;
        return false;
    }

    // Read output
    char buffer[128];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    int status = pclose(pipe);

    if (status == 0 || WEXITSTATUS(status) == 0) {
        // Command succeeded
        outputs_["stdout"] = output;
        outputs_["exit_code"] = "0";
        return true;
    } else {
        // Command failed
        last_error_ = "Command failed with exit code: " +
                      std::to_string(WEXITSTATUS(status));
        outputs_["stdout"] = output;
        outputs_["exit_code"] = std::to_string(WEXITSTATUS(status));
        return false;
    }
}

bool Node::execute_with_timeout(int timeout_ms, std::function<bool()> executor) {
    if (timeout_ms <= 0) {
        // No timeout, execute directly
        return executor();
    }

    // Execute with timeout using thread
    bool result = false;
    bool completed = false;
    std::exception_ptr exception = nullptr;

    auto task = [&]() {
        try {
            result = executor();
            completed = true;
        } catch (...) {
            exception = std::current_exception();
            completed = true;
        }
    };

    std::thread worker(task);
    worker.detach();

    // Wait for completion or timeout
    auto start = std::chrono::steady_clock::now();
    while (!completed) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start).count();

        if (elapsed >= timeout_ms) {
            last_error_ = "Execution timed out after " +
                          std::to_string(timeout_ms) + "ms";
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (exception) {
        std::rethrow_exception(exception);
    }

    return result;
}

} // namespace framework
} // namespace agentflow
