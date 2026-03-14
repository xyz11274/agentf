/**
 * @file test_system_agent_mock_e2e.cpp
 * @brief E2E test for SystemAgentMock demonstrating dispatcher-skill-coordinator flow
 *
 * This test verifies the following message flow:
 * 1. User message triggers dispatcher
 * 2. Dispatcher launches skill-selection skill (from next_skills config)
 * 3. skill-selection randomly picks 2 skills from [travel, shopping, foodie]
 * 4. Selected skills execute and store results in DataBus
 * 5. Coordinator aggregates results from all skills
 * 6. User receives aggregated response
 *
 * Multi-dialogue support:
 * 7. Multiple dialogues can be processed in parallel in the same session
 * 8. Each dialogue has a unique dialogue_id generated from hash(user_text + message_id)
 * 9. Data is isolated per dialogue using dialogue_id prefix
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <future>
#include <mutex>

#include "agent/agent.h"
#include "agent/session.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"
#include "runtime/databus/node_databus_adapter.h"
#include "runtime/databus/node_databus_client.h"
#include "agentflow/so_node.h"

using namespace agentflow;

std::string find_agent_path() {
    std::vector<std::string> search_paths = {
        "examples/agent/system_agent_mock",
        "../examples/agent/system_agent_mock",
        "../../examples/agent/system_agent_mock",
        "../../../examples/agent/system_agent_mock",
        "/root/projects/AgentFlow/examples/agent/system_agent_mock"
    };

    for (const auto& path : search_paths) {
        std::filesystem::path full_path = std::filesystem::absolute(path);
        if (std::filesystem::exists(full_path / "package.json")) {
            return std::filesystem::canonical(full_path).string();
        }
    }
    return "";
}

struct TestResult {
    bool success = false;
    std::string response;
    std::string error;
};

TestResult run_single_dialogue_test(
    const std::shared_ptr<framework::MMAgent>& agent,
    const std::string& message_id,
    const std::string& content) {

    TestResult result;

    // Construct user request message
    framework::MMAgentMsgQue::Message message;
    message.message_id = message_id;
    message.metadata["message_type"] = "intent_request";
    message.metadata["user_id"] = "test_user";
    message.metadata["client_id"] = "test_client";
    message.content = content;

    // Route message through agent's message interface
    if (!agent->route_request_message(message)) {
        result.error = "Failed to route message";
        return result;
    }

    // Wait for response
    const int max_wait_seconds = 30;
    for (int i = 0; i < max_wait_seconds * 10; ++i) {
        // Process messages to drive workflow forward
        agent->process_all_sessions_until_idle();

        // Check for response
        framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            if (!response.content.empty()) {
                result.response = response.content;
                result.success = true;
                return result;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    result.error = "No response received within " + std::to_string(max_wait_seconds) + " seconds";
    return result;
}

int main() {
    std::cout << "=== SystemAgentMock E2E Test ===" << std::endl;
    std::cout << std::endl;

    // Find agent package path
    std::string package_path = find_agent_path();
    if (package_path.empty()) {
        std::cerr << "ERROR: SystemAgentMock package not found" << std::endl;
        return 1;
    }
    std::cout << "Agent path: " << package_path << std::endl;

    // Check that SO libraries exist
    std::vector<std::string> required_libs = {
        "lib/libskill_selection_node.so",
        "lib/libtravel_node.so",
        "lib/libshopping_node.so",
        "lib/libfoodie_node.so",
        "lib/libcoordinator_node.so"
    };
    for (const auto& lib : required_libs) {
        if (!std::filesystem::exists(std::filesystem::path(package_path) / lib)) {
            std::cerr << "ERROR: Required SO library not found: " << lib << std::endl;
            std::cerr << "Please build the SO libraries first with: ./build.sh build" << std::endl;
            return 1;
        }
    }
    std::cout << "All SO libraries found" << std::endl;

    // Save and change working directory
    std::string original_dir = std::filesystem::current_path().string();
    std::filesystem::current_path(package_path);
    std::cout << "Working directory: " << package_path << std::endl;

    // Create agent from package
    auto agent = framework::MMAgent::create_from_package(".");
    if (!agent) {
        std::cerr << "ERROR: Failed to create agent" << std::endl;
        std::filesystem::current_path(original_dir);
        return 1;
    }
    std::cout << "Agent: " << agent->get_name() << std::endl;

    // Initialize agent
    if (!agent->initialize()) {
        std::cerr << "ERROR: Failed to initialize agent" << std::endl;
        std::filesystem::current_path(original_dir);
        return 1;
    }
    std::cout << "Agent initialized" << std::endl;

    // Create and start runtime
    auto runtime = std::make_unique<runtime::MMAgentRuntime>();
    if (!runtime->start()) {
        std::cerr << "ERROR: Failed to start runtime" << std::endl;
        std::filesystem::current_path(original_dir);
        return 1;
    }

    // Register agent
    if (!runtime->register_agent(agent)) {
        std::cerr << "ERROR: Failed to register agent" << std::endl;
        runtime->stop();
        std::filesystem::current_path(original_dir);
        return 1;
    }

    // Start agent
    if (!agent->start()) {
        std::cerr << "ERROR: Failed to start agent" << std::endl;
        runtime->stop();
        std::filesystem::current_path(original_dir);
        return 1;
    }
    std::cout << "Agent started" << std::endl;

    int tests_passed = 0;
    int tests_failed = 0;

    // ============================================================
    // Test 1: Basic single dialogue flow
    // ============================================================
    std::cout << std::endl;
    std::cout << "=== Test 1: Single Dialogue Flow ===" << std::endl;

    // Construct user request message
    framework::MMAgentMsgQue::Message message;
    message.message_id = "test-msg-system-mock-001";
    message.metadata["message_type"] = "intent_request";
    message.metadata["user_id"] = "test_user";
    message.metadata["client_id"] = "test_client";
    message.content = "What are the best travel, shopping, and food options?";

    std::cout << std::endl;
    std::cout << "=== Sending Message ===" << std::endl;
    std::cout << "Content: " << message.content << std::endl;
    std::cout << std::endl;

    // Route message through agent's message interface
    if (!agent->route_request_message(message)) {
        std::cerr << "ERROR: Failed to route message" << std::endl;
        agent->stop();
        runtime->stop();
        std::filesystem::current_path(original_dir);
        return 1;
    }
    std::cout << "Message routed successfully" << std::endl;

    // Wait for response
    std::cout << std::endl;
    std::cout << "=== Waiting for Response ===" << std::endl;

    bool got_response = false;
    std::string response_content;
    const int max_wait_seconds = 30;

    for (int i = 0; i < max_wait_seconds * 10; ++i) {
        // Process messages to drive workflow forward
        agent->process_all_sessions_until_idle();

        // Check for response
        framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            if (!response.content.empty()) {
                response_content = response.content;
                got_response = true;
                break;
            }
        }

        if (got_response) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Output result
    std::cout << std::endl;
    if (got_response) {
        std::cout << "=== Response Received ===" << std::endl;
        std::cout << response_content << std::endl;

        // Verify that the response contains aggregated content from multiple skills
        bool has_travel = response_content.find("Travel") != std::string::npos ||
                          response_content.find("travel") != std::string::npos;
        bool has_shopping = response_content.find("Shopping") != std::string::npos ||
                            response_content.find("shopping") != std::string::npos;
        bool has_foodie = response_content.find("Foodie") != std::string::npos ||
                          response_content.find("food") != std::string::npos;

        std::cout << std::endl;
        std::cout << "=== Verification ===" << std::endl;
        std::cout << "Contains Travel content: " << (has_travel ? "YES" : "NO") << std::endl;
        std::cout << "Contains Shopping content: " << (has_shopping ? "YES" : "NO") << std::endl;
        std::cout << "Contains Foodie content: " << (has_foodie ? "YES" : "NO") << std::endl;

        // At least 2 skills should have contributed (skill-selection picks 2)
        int skills_contributed = (has_travel ? 1 : 0) + (has_shopping ? 1 : 0) + (has_foodie ? 1 : 0);
        std::cout << "Skills contributed: " << skills_contributed << std::endl;

        if (skills_contributed >= 2) {
            std::cout << "Test 1: PASSED" << std::endl;
            tests_passed++;
        } else {
            std::cerr << "WARNING: Expected at least 2 skills to contribute, got " << skills_contributed << std::endl;
            std::cout << "Test 1: FAILED" << std::endl;
            tests_failed++;
        }
    } else {
        std::cerr << "ERROR: No response received within " << max_wait_seconds << " seconds" << std::endl;
        std::cout << "Test 1: FAILED" << std::endl;
        tests_failed++;
    }

    // ============================================================
    // Test 2: Multi-dialogue parallel processing
    // ============================================================
    std::cout << std::endl;
    std::cout << "=== Test 2: Multi-Dialogue Parallel Processing ===" << std::endl;
    std::cout << "Sending two different queries in parallel..." << std::endl;

    // Launch two dialogues in parallel using async
    auto future1 = std::async(std::launch::async, [&]() {
        return run_single_dialogue_test(agent, "msg-parallel-001", "I want to travel to Paris");
    });

    auto future2 = std::async(std::launch::async, [&]() {
        return run_single_dialogue_test(agent, "msg-parallel-002", "Find me the best shopping deals");
    });

    // Process both dialogues
    for (int i = 0; i < max_wait_seconds * 10; ++i) {
        agent->process_all_sessions_until_idle();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Get results
    auto result1 = future1.get();
    auto result2 = future2.get();

    std::cout << std::endl;
    std::cout << "=== Parallel Dialogue Results ===" << std::endl;

    if (result1.success) {
        std::cout << "Dialogue 1 (travel) response:" << std::endl;
        std::cout << result1.response.substr(0, 100) << "..." << std::endl;
        std::cout << "Test 2a: PASSED" << std::endl;
        tests_passed++;
    } else {
        std::cerr << "Dialogue 1 failed: " << result1.error << std::endl;
        std::cout << "Test 2a: FAILED" << std::endl;
        tests_failed++;
    }

    if (result2.success) {
        std::cout << "Dialogue 2 (shopping) response:" << std::endl;
        std::cout << result2.response.substr(0, 100) << "..." << std::endl;
        std::cout << "Test 2b: PASSED" << std::endl;
        tests_passed++;
    } else {
        std::cerr << "Dialogue 2 failed: " << result2.error << std::endl;
        std::cout << "Test 2b: FAILED" << std::endl;
        tests_failed++;
    }

    // ============================================================
    // Test 3: Sequential dialogues in same session
    // ============================================================
    std::cout << std::endl;
    std::cout << "=== Test 3: Sequential Dialogues ===" << std::endl;
    std::cout << "Sending sequential queries..." << std::endl;

    auto seq_result1 = run_single_dialogue_test(agent, "msg-seq-001", "First query about travel");
    auto seq_result2 = run_single_dialogue_test(agent, "msg-seq-002", "Second query about food");

    // Process both dialogues
    for (int i = 0; i < max_wait_seconds * 10; ++i) {
        agent->process_all_sessions_until_idle();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Check if we got both responses
        framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            // Drain responses
        }
    }

    if (seq_result1.success) {
        std::cout << "Sequential dialogue 1: PASSED" << std::endl;
        tests_passed++;
    } else {
        std::cout << "Sequential dialogue 1: FAILED - " << seq_result1.error << std::endl;
        tests_failed++;
    }

    if (seq_result2.success) {
        std::cout << "Sequential dialogue 2: PASSED" << std::endl;
        tests_passed++;
    } else {
        std::cout << "Sequential dialogue 2: FAILED - " << seq_result2.error << std::endl;
        tests_failed++;
    }

    // Cleanup
    std::cout << std::endl;
    std::cout << "=== Cleanup ===" << std::endl;
    agent->stop();
    runtime->stop();
    std::filesystem::current_path(original_dir);

    std::cout << std::endl;
    std::cout << "=== Test Summary ===" << std::endl;
    std::cout << "Tests passed: " << tests_passed << std::endl;
    std::cout << "Tests failed: " << tests_failed << std::endl;

    bool all_passed = tests_failed == 0;
    std::cout << std::endl;
    std::cout << "=== Overall: " << (all_passed ? "PASSED" : "FAILED") << " ===" << std::endl;
    return all_passed ? 0 : 1;
}