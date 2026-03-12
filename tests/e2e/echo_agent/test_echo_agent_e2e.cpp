/**
 * @file test_echo_agent.cpp
 * @brief E2E smoke test for the merged echo workflow routing and response delivery
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>

#include "agent/agent.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"

using namespace agentflow;

std::string find_echo_workflow_package_path() {
    // Get the directory where the test executable is located
    std::string exe_dir;
    std::filesystem::path current_exe = std::filesystem::current_path();

    // Try multiple search strategies
    std::vector<std::string> search_paths = {
        // From project root
        "examples/agent/llm_query_agent",
        // From build directory
        "../examples/agent/llm_query_agent",
        // From build/tests/e2e directory
        "../../examples/agent/llm_query_agent",
        // From build/tests directory
        "../../../examples/agent/llm_query_agent"
    };

    for (const auto& path : search_paths) {
        std::filesystem::path full_path = std::filesystem::absolute(path);
        if (std::filesystem::exists(full_path / "package.json")) {
            // Canonicalize to remove any ".." or "." components
            return std::filesystem::canonical(full_path).string();
        }
    }
    return "";
}

int main() {
    std::cout << "=== Echo Agent Test ===" << std::endl;

    // 1. Find echo agent package path (already returns canonical path)
    std::string package_path = find_echo_workflow_package_path();
    if (package_path.empty()) {
        std::cerr << "ERROR: merged echo workflow package not found" << std::endl;
        return 1;
    }

    std::cout << "Found merged echo workflow package at: " << package_path << std::endl;

    // Save current directory and change to package directory
    // This is needed so that shell scripts in workflows can be found
    std::string original_dir = std::filesystem::current_path().string();
    std::filesystem::current_path(package_path);
    std::cout << "Changed working directory to: " << package_path << std::endl;

    // 2. Create agent from package
    auto agent = framework::MMAgent::create_from_package(".");
    if (!agent) {
        std::cerr << "ERROR: Failed to create agent from package" << std::endl;
        return 1;
    }
    std::cout << "Agent created: " << agent->get_name() << std::endl;

    // 3. Initialize agent
    if (!agent->initialize()) {
        std::cerr << "ERROR: Failed to initialize agent" << std::endl;
        return 1;
    }
    std::cout << "Agent initialized" << std::endl;

    // 4. Create runtime
    auto runtime = std::make_unique<runtime::MMAgentRuntime>();
    if (!runtime->start()) {
        std::cerr << "ERROR: Failed to start runtime" << std::endl;
        return 1;
    }
    std::cout << "Runtime started" << std::endl;

    // 5. Register agent to runtime
    if (!runtime->register_agent(agent)) {
        std::cerr << "ERROR: Failed to register agent to runtime" << std::endl;
        runtime->stop();
        return 1;
    }
    std::cout << "Agent registered to runtime" << std::endl;

    // 6. Start agent
    if (!agent->start()) {
        std::cerr << "ERROR: Failed to start agent" << std::endl;
        runtime->stop();
        return 1;
    }
    std::cout << "Agent started" << std::endl;

    // 7. Construct request message
    framework::MMAgentMsgQue::Message message;
    message.message_id = "test-msg-001";
    message.metadata["message_type"] = "intent_request";
    message.metadata["user_id"] = "test_user";
    message.metadata["client_id"] = "test_client";
    message.content = "Foo";
    message.metadata["workflow_id"] = "echo-workflow";

    std::cout << "\nSending message: '" << message.content << "'" << std::endl;

    // 8. Route request message
    if (!agent->route_request_message(message)) {
        std::cerr << "ERROR: Failed to route request message" << std::endl;
        agent->stop();
        runtime->stop();
        return 1;
    }
    std::cout << "Message routed successfully" << std::endl;

    // 9. Process messages and wait for response
    std::cout << "\nWaiting for response..." << std::endl;

    bool got_response = false;
    const int max_wait_iterations = 100;
    for (int i = 0; i < max_wait_iterations; ++i) {
        // Process session messages
        agent->process_all_sessions_until_idle();

        // Drain any available responses; the merged package can take a couple of
        // seconds because workflow execution still goes through dispatcher plumbing.
        framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            if (response.content.empty()) {
                continue;
            }
            std::cout << "\n=== Response Received ===" << std::endl;
            std::cout << "Message ID: " << response.message_id << std::endl;
            std::cout << "Content: " << response.content << std::endl;
            got_response = true;
            break;
        }

        if (got_response) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!got_response) {
        std::cerr << "ERROR: No response received within timeout" << std::endl;
        std::cerr << "Response queue size: " << agent->get_response_queue_size() << std::endl;
    }

    // 10. Cleanup
    std::cout << "\nCleaning up..." << std::endl;
    agent->stop();
    runtime->stop();

    // Restore original working directory
    std::filesystem::current_path(original_dir);

    std::cout << "\n=== Test " << (got_response ? "PASSED" : "FAILED") << " ===" << std::endl;
    return got_response ? 0 : 1;
}
