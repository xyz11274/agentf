#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent.h"

using namespace agentflow::framework;

int main(int argc, char* argv[]) {
    std::cout << "=== AgentFlow Agent Package Loading Example ===" << std::endl;

    std::string package_path;
    if (argc > 1) {
        package_path = argv[1];
    } else {
        // Default to the basic_agent example
        // In a real scenario, this should be an absolute path or relative to the execution context
        // We will assume the example is run from the project root
        package_path = "examples/agent/basic_agent";
    }

    std::cout << "Loading agent from package: " << package_path << std::endl;

    // Create agent from package
    auto agent = Agent::create_from_package(package_path);

    if (!agent) {
        std::cerr << "Failed to load agent from package: " << package_path << std::endl;
        return 1;
    }

    std::cout << "\nAgent Loaded Successfully!" << std::endl;
    std::cout << "--------------------------" << std::endl;
    std::cout << "Name: " << agent->get_name() << std::endl;
    std::cout << "ID: " << agent->get_id() << std::endl;
    std::cout << "Version: " << agent->get_config().version << std::endl;
    std::cout << "Description: " << agent->get_config().description << std::endl;

    // Initialize and run the agent
    std::cout << "\nInitializing Agent..." << std::endl;
    if (agent->initialize()) {
        std::cout << "Agent initialized." << std::endl;
    } else {
        std::cerr << "Agent initialization failed!" << std::endl;
        return 1;
    }

    std::cout << "Starting Agent..." << std::endl;
    if (agent->start()) {
        std::cout << "Agent started." << std::endl;
    } else {
        std::cerr << "Agent start failed!" << std::endl;
        return 1;
    }
    
    // In a real application, we would wait here or have an event loop
    // For this example, we just stop it
    std::cout << "Stopping Agent..." << std::endl;
    if (agent->stop()) {
        std::cout << "Agent stopped." << std::endl;
    } else {
        std::cerr << "Agent stop failed!" << std::endl;
        return 1;
    }

    std::cout << "\n=== Example Complete ===" << std::endl;

    return 0;
}
