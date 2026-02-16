// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating Node execution functionality

#include "agentflow/framework/node.h"
#include "agentflow/basic/logger.h"
#include <iostream>

using namespace agentflow;
using namespace agentflow::framework;

int main() {
    Logger::instance().info("AgentFlow Node Execution Example");
    Logger::instance().info("================================");

    // Example 1: C++ function execution
    Logger::instance().info("\n--- Example 1: C++ Function Execution ---");

    Node::Config cpp_config;
    cpp_config.id = "node_cpp_1";
    cpp_config.name = "Data Processor";
    cpp_config.type = "processor";
    cpp_config.priority = 0;
    cpp_config.execution_mode = Node::ExecutionMode::CPP_FUNCTION;
    cpp_config.timeout_ms = 5000;
    cpp_config.max_retries = 2;

    Node cpp_node(cpp_config);

    if (!cpp_node.initialize()) {
        Logger::instance().error("Failed to initialize C++ node");
        return 1;
    }

    if (cpp_node.execute()) {
        Logger::instance().info("C++ node executed successfully");
        Logger::instance().info_f("Node state: %d",
                                static_cast<int>(cpp_node.get_state()));
    } else {
        Logger::instance().error("C++ node execution failed");
    }

    cpp_node.cleanup();

    // Example 2: Shell command execution
    Logger::instance().info("\n--- Example 2: Shell Command Execution ---");

    Node::Config shell_config;
    shell_config.id = "node_shell_1";
    shell_config.name = "List Files";
    shell_config.type = "executor";
    shell_config.priority = 1;
    shell_config.execution_mode = Node::ExecutionMode::SHELL_SCRIPT;
    shell_config.command = "ls -la";
    shell_config.timeout_ms = 10000;
    shell_config.max_retries = 1;

    Node shell_node(shell_config);

    if (!shell_node.initialize()) {
        Logger::instance().error("Failed to initialize shell node");
        return 1;
    }

    if (shell_node.execute()) {
        Logger::instance().info("Shell node executed successfully");
        Logger::instance().info_f("Node state: %d",
                                static_cast<int>(shell_node.get_state()));

        // Print output
        const auto& outputs = shell_node.get_outputs();
        auto it = outputs.find("stdout");
        if (it != outputs.end()) {
            Logger::instance().info("Command output:");
            std::visit([](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    Logger::instance().info(arg);
                } else {
                    Logger::instance().info_f("%s", std::to_string(arg).c_str());
                }
            }, it->second);
        }
    } else {
        Logger::instance().error("Shell node execution failed");
    }

    shell_node.cleanup();

    // Example 3: Echo command execution
    Logger::instance().info("\n--- Example 3: Echo Command Execution ---");

    Node::Config echo_config;
    echo_config.id = "node_echo_1";
    echo_config.name = "Echo Test";
    echo_config.type = "executor";
    echo_config.priority = 2;
    echo_config.execution_mode = Node::ExecutionMode::SHELL_SCRIPT;
    echo_config.command = "echo 'Hello from AgentFlow Node!'";
    echo_config.timeout_ms = 5000;
    echo_config.max_retries = 0;

    Node echo_node(echo_config);

    if (!echo_node.initialize()) {
        Logger::instance().error("Failed to initialize echo node");
        return 1;
    }

    if (echo_node.execute()) {
        Logger::instance().info("Echo node executed successfully");

        const auto& outputs = echo_node.get_outputs();
        auto it = outputs.find("stdout");
        if (it != outputs.end()) {
            Logger::instance().info("Echo output:");
            std::visit([](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    Logger::instance().info(arg);
                } else {
                    Logger::instance().info_f("%s", std::to_string(arg).c_str());
                }
            }, it->second);
        }
    } else {
        Logger::instance().error("Echo node execution failed");
    }

    echo_node.cleanup();

    Logger::instance().info("\n================================");
    Logger::instance().info("Example completed successfully!");
    return 0;
}
