/**
 * @file test_llm_query_agent.cpp
 * @brief Black-box E2E test for LLM Query Agent
 *
 * Tests the agent through its message routing interface:
 * User Message -> Dispatcher -> Coordinator -> Skill (LLM Query)
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <cstdlib>

#include "agent/agent.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"

using namespace agentflow;

std::string find_agent_path() {
    std::vector<std::string> search_paths = {
        "examples/agent/llm_query_agent",
        "../examples/agent/llm_query_agent",
        "../../examples/agent/llm_query_agent",
        "../../../examples/agent/llm_query_agent",
        "/root/projects/AgentFlow/examples/agent/llm_query_agent"
    };

    for (const auto& path : search_paths) {
        std::filesystem::path full_path = std::filesystem::absolute(path);
        if (std::filesystem::exists(full_path / "package.json")) {
            return std::filesystem::canonical(full_path).string();
        }
    }
    return "";
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [prompt]" << std::endl;
    std::cerr << "  prompt  User message to send to the agent (default: test message)" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Environment variables:" << std::endl;
    std::cerr << "  LLM_API_KEY   API key for LLM service" << std::endl;
    std::cerr << "  LLM_MODEL     Model name (default: qwen3.5-plus)" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        print_usage(argv[0]);
        return 0;
    }

    // Get prompt from argument or use default
    std::string prompt = (argc > 1) ? argv[1] : "What is 2+2?";

    std::cout << "=== LLM Query Agent E2E Test ===" << std::endl;

    // Check API key
    const char* api_key = std::getenv("LLM_API_KEY");
    if (!api_key || std::string(api_key).empty()) {
        std::cerr << "Warning: LLM_API_KEY not set, using test key" << std::endl;
    }
    std::cout << "Prompt: " << prompt << std::endl;
    std::cout << std::endl;

    // Find agent package path
    std::string package_path = find_agent_path();
    if (package_path.empty()) {
        std::cerr << "ERROR: LLM Query Agent package not found" << std::endl;
        return 1;
    }
    std::cout << "Agent path: " << package_path << std::endl;

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

    // Inject LLM configuration from environment into the agent's DataBus
    // This allows SO library nodes to access LLM config via INodeDataBus
    {
        auto& bus = runtime->get_databus(agent->get_id());
        const char* llm_api_key = std::getenv("LLM_API_KEY");
        if (llm_api_key && llm_api_key[0] != '\0') {
            bus.put("api_key", DataVariant(std::string(llm_api_key)));
            std::cout << "Injected LLM_API_KEY into agent databus" << std::endl;
        }
        const char* llm_model = std::getenv("LLM_MODEL");
        if (llm_model && llm_model[0] != '\0') {
            bus.put("model", DataVariant(std::string(llm_model)));
        }
        const char* llm_base_url = std::getenv("LLM_BASE_URL");
        if (llm_base_url && llm_base_url[0] != '\0') {
            bus.put("base_url", DataVariant(std::string(llm_base_url)));
        }
    }

    // Start agent
    if (!agent->start()) {
        std::cerr << "ERROR: Failed to start agent" << std::endl;
        runtime->stop();
        std::filesystem::current_path(original_dir);
        return 1;
    }
    std::cout << "Agent started" << std::endl;
    std::cout << std::endl;

    // Construct user request message (black-box test through message interface)
    framework::MMAgentMsgQue::Message message;
    message.message_id = "test-msg-llm-001";
    message.metadata["message_type"] = "intent_request";
    message.metadata["user_id"] = "test_user";
    message.metadata["client_id"] = "test_client";
    message.content = prompt;
    message.metadata["workflow_id"] = "llm-query-dispatcher-workflow";

    std::cout << "=== Sending Message ===" << std::endl;
    std::cout << "Content: " << message.content << std::endl;
    std::cout << std::endl;

    // Route message through agent's message interface
    // This goes through: Dispatcher -> Coordinator -> Skill (LLM Query)
    if (!agent->route_request_message(message)) {
        std::cerr << "ERROR: Failed to route message" << std::endl;
        agent->stop();
        runtime->stop();
        std::filesystem::current_path(original_dir);
        return 1;
    }
    std::cout << "Message routed successfully" << std::endl;

    // Wait for skill workflow to complete and get LLM response from databus
    std::cout << std::endl;
    std::cout << "=== Waiting for LLM Response ===" << std::endl;

    bool got_response = false;
    std::string response_content;
    const int max_wait_seconds = 120;  // LLM can be slow

    for (int i = 0; i < max_wait_seconds * 10; ++i) {
        // Process messages to drive workflow forward
        agent->process_all_sessions_until_idle();

        // Check if skill workflow has published a response to databus
        DataVariant response_value;
        if (agent->get_data("response", response_value)) {
            if (auto* text = std::get_if<std::string>(&response_value)) {
                if (!text->empty()) {
                    response_content = *text;
                    got_response = true;
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Output result
    std::cout << std::endl;
    if (got_response) {
        std::cout << "=== LLM Response ===" << std::endl;
        std::cout << response_content << std::endl;
    } else {
        std::cerr << "ERROR: No response received within " << max_wait_seconds << " seconds" << std::endl;

        // Debug: check what's in the databus
        DataVariant status_val, error_val;
        if (agent->get_data("status", status_val)) {
            if (auto* s = std::get_if<std::string>(&status_val)) {
                std::cerr << "Status: " << *s << std::endl;
            }
        }
        if (agent->get_data("error", error_val)) {
            if (auto* s = std::get_if<std::string>(&error_val)) {
                std::cerr << "Error: " << *s << std::endl;
            }
        }
    }

    // Cleanup
    std::cout << std::endl;
    std::cout << "=== Cleanup ===" << std::endl;
    agent->stop();
    runtime->stop();
    std::filesystem::current_path(original_dir);

    std::cout << std::endl;
    std::cout << "=== Test " << (got_response ? "PASSED" : "FAILED") << " ===" << std::endl;
    return got_response ? 0 : 1;
}