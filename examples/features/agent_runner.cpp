// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// MMAgent Runner - Standalone executable that loads and executes MMAgent packages
// Usage: agent_runner <package_path> [workflow_id] [--execute] [--interactive]

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "agent/package.h"
#include "interactive_runner.h"
#include "runtime/agent_runtime.h"
#include "runtime/worker.h"
#include "workflow/dsl/workflow_parser.h"

using namespace agentflow;

namespace {

std::string get_executable_dir(const char* argv0) {
    std::string path(argv0);
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(0, pos);
    }
    return ".";
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " <package_path> [options]\n"
              << "\n"
              << "Options:\n"
              << "  --list-workflows    List all workflows in the package\n"
              << "  --execute [wf_id]   Execute workflow (default: first workflow)\n"
              << "  --interactive       Start interactive mode (message-driven execution)\n"
              << "  --validate          Validate package structure\n"
              << "  --output-file FILE  Write output to file\n"
              << "  --verbose           Enable verbose output\n"
              << "  --help              Show this help\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " examples/agent/nested_workflow_agent --list-workflows\n"
              << "  " << program << " examples/agent/nested_workflow_agent --execute main-workflow\n"
              << "  " << program << " examples/agent/nested_workflow_agent --interactive\n";
}

// Capture output from script execution
std::string g_captured_output;
std::ofstream g_output_file;

void capture_output(const std::string& message) {
    g_captured_output += message + "\n";
    std::cout << message << std::endl;
    if (g_output_file.is_open()) {
        g_output_file << message << std::endl;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string package_path = argv[1];
    std::string workflow_id;
    std::string output_file_path;
    bool list_workflows = false;
    bool execute = false;
    bool validate = false;
    bool interactive = false;
    bool verbose = false;

    // Parse arguments
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--list-workflows") {
            list_workflows = true;
        } else if (arg == "--execute") {
            execute = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                workflow_id = argv[++i];
            }
        } else if (arg == "--interactive") {
            interactive = true;
        } else if (arg == "--validate") {
            validate = true;
        } else if (arg == "--output-file") {
            if (i + 1 < argc) {
                output_file_path = argv[++i];
            }
        } else if (arg == "--verbose") {
            verbose = true;
        }
    }

    // Open output file if specified
    if (!output_file_path.empty()) {
        g_output_file.open(output_file_path);
        if (!g_output_file.is_open()) {
            std::cerr << "Error: Cannot open output file: " << output_file_path << std::endl;
            return 1;
        }
    }

    // Resolve package path
    if (!std::filesystem::exists(package_path)) {
        // Try relative to executable
        std::string abs_path = get_executable_dir(argv[0]) + "/" + package_path;
        if (std::filesystem::exists(abs_path)) {
            package_path = abs_path;
        } else {
            // Try project root relative
            abs_path = get_executable_dir(argv[0]) + "/../" + package_path;
            if (std::filesystem::exists(abs_path)) {
                package_path = abs_path;
            }
        }
    }

    if (!std::filesystem::exists(package_path + "/package.json")) {
        std::cerr << "Error: Package not found at: " << package_path << std::endl;
        return 1;
    }

    capture_output("=== AgentFlow MMAgent Runner ===");
    capture_output("Package: " + package_path);

    // 1. Load MMAgent Package
    auto package = framework::AgentPackage::load_from_directory(package_path);
    if (!package) {
        capture_output("ERROR: Failed to load agent package");
        return 1;
    }

    const auto& pkg_config = package->get_config();
    capture_output("Package: " + pkg_config.package_name + " v" + pkg_config.version);

    // 2. Validate mode
    if (validate) {
        capture_output("\n=== Package Validation ===");
        capture_output("  Name: " + pkg_config.package_name);
        capture_output("  Version: " + pkg_config.version);
        capture_output("  Description: " + pkg_config.description);
        capture_output("  Workflows: " + std::to_string(pkg_config.workflow_files.size()));
        for (const auto& wf : pkg_config.workflow_files) {
            capture_output("    - " + wf);
        }
        capture_output("  Entry Point: " + pkg_config.entry_point);
        capture_output("  Session Tasks: " + std::to_string(pkg_config.session_tasks.size()));
        capture_output("\n[OK] Package validation passed");
        return 0;
    }

    // 3. Create and initialize MMAgent
    auto agent = framework::MMAgent::create_from_package(package_path);
    if (!agent) {
        capture_output("ERROR: Failed to create agent");
        return 1;
    }

    if (!agent->initialize()) {
        capture_output("ERROR: Failed to initialize agent");
        return 1;
    }

    capture_output("MMAgent: " + agent->get_name() + " initialized");

    // 4. List workflows mode
    if (list_workflows) {
        capture_output("\n=== Workflows ===");
        auto workflows = agent->get_workflows();
        for (const auto& wf : workflows) {
            capture_output("  [" + wf->get_id() + "] " + wf->get_name() +
                          " (" + std::to_string(wf->get_nodes().size()) + " nodes)");

            // Show nested workflow references
            for (const auto& node : wf->get_nodes()) {
                if (node->get_type() == "workflow") {
                    capture_output("    └─> workflow_ref: " + node->get_config().workflow_ref);
                }
            }
        }
        return 0;
    }

    // 5. Execute mode
    if (execute) {
        // Default to first workflow if not specified
        if (workflow_id.empty()) {
            if (!pkg_config.workflow_files.empty()) {
                // Use first workflow file name as default
                workflow_id = std::filesystem::path(pkg_config.workflow_files[0]).stem().string();
            }
        }

        capture_output("\n=== Executing MMWorkflow: " + workflow_id + " ===");

        auto workflow = agent->get_workflow(workflow_id);
        if (!workflow) {
            capture_output("ERROR: MMWorkflow not found: " + workflow_id);
            return 1;
        }

        // Get topological order
        auto topo_order = workflow->get_topological_order();
        capture_output("Execution order: " + std::to_string(topo_order.size()) + " nodes");

        // Execute nodes in topological order
        std::string script_dir = std::filesystem::absolute(package_path).string();
        int executed_count = 0;
        int success_count = 0;

        capture_output("\n--- Execution Output ---");

        for (const auto& node_id : topo_order) {
            auto node = workflow->get_node(node_id);
            if (!node) continue;

            // Skip start/end nodes
            if (node->get_type() == "start" || node->get_type() == "end") {
                capture_output("[SKIP] " + node_id + " (" + node->get_type() + ")");
                continue;
            }

            // Only execute shell_script nodes
            if (node->get_execution_mode() == framework::MMNode::ExecutionMode::SHELL_SCRIPT) {
                std::string script_path = script_dir + "/" + node->get_command();

                capture_output("[EXEC] " + node_id + ": " + node->get_command());

                if (std::filesystem::exists(script_path)) {
                    runtime::MMWorkerEngine::ExecutionRequest request;
                    request.mode = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
                    request.command = script_path;
                    request.working_dir = script_dir;
                    request.timeout_ms = 5000;

                    auto response = runtime::MMWorkerEngine::execute_code(request);
                    executed_count++;

                    if (response.success) {
                        success_count++;
                        // Output the script stdout
                        std::istringstream iss(response.stdout_content);
                        std::string line;
                        while (std::getline(iss, line)) {
                            capture_output("  >> " + line);
                        }
                    } else {
                        capture_output("  [ERROR] " + response.error_message);
                    }
                } else {
                    capture_output("  [WARN] Script not found: " + script_path);
                }
            } else if (node->get_type() == "workflow") {
                // Nested workflow reference
                capture_output("[NESTED] " + node_id + " -> " + node->get_config().workflow_ref);
                // In a full implementation, this would recursively execute the nested workflow
            } else {
                capture_output("[SKIP] " + node_id + " (type: " + node->get_type() + ")");
            }
        }

        capture_output("\n--- Execution Summary ---");
        capture_output("Executed: " + std::to_string(executed_count) + " nodes");
        capture_output("Success: " + std::to_string(success_count) + " nodes");
        capture_output("\n[OK] MMWorkflow execution completed");

        return (success_count == executed_count) ? 0 : 1;
    }

    // 6. Interactive mode - Message-driven execution
    if (interactive) {
        capture_output("\n=== Starting Interactive Mode ===");
        capture_output("Using message-driven architecture through MMAgent framework");
        capture_output("");

        InteractiveRunner::Config runner_config;
        runner_config.verbose = verbose;

        InteractiveRunner runner(package_path, runner_config);
        if (!runner.initialize()) {
            capture_output("ERROR: Failed to initialize interactive runner");
            return 1;
        }

        runner.run();
        return 0;
    }

    // Default: show info
    capture_output("\nUse --list-workflows, --execute, or --interactive to interact with the agent");
    return 0;
}