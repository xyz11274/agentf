// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating DSL parsing functionality

#include <iostream>

#include "utils/logger.h"
#include "workflow/dsl/workflow_parser.h"

using namespace agentflow;
using namespace agentflow::dsl;

int main() {
  Logger::instance().info("AgentFlow DSL Parser Example");
  Logger::instance().info("===============================");

  // Example Workflow DSL (JSON format)
  std::string workflow_dsl = R"({
        "dsl_version": "1.0.0",
        "id": "example_workflow",
        "name": "Simple Processing Workflow",
        "version": "1.0",
        "description": "A simple example workflow for testing DSL parsing",
        "metadata": {
            "author": "AgentFlow Team",
            "created": "2026-01-01"
        },
        "nodes": [
            {
                "id": "node1",
                "name": "Data Ingestion",
                "type": "ingestor",
                "priority": 0,
                "max_retries": 3,
                "timeout_ms": 30000,
                "inputs": ["input_data"],
                "outputs": ["processed_data"]
            },
            {
                "id": "node2",
                "name": "Data Processing",
                "type": "processor",
                "priority": 1,
                "max_retries": 2,
                "timeout_ms": 60000,
                "inputs": ["processed_data"],
                "outputs": ["output_data"]
            },
            {
                "id": "node3",
                "name": "Data Export",
                "type": "exporter",
                "priority": 0,
                "max_retries": 1,
                "timeout_ms": 15000,
                "inputs": ["output_data"],
                "outputs": []
            }
        ],
        "edges": [
            {
                "id": "edge1",
                "source": "node1",
                "target": "node2",
                "transfer_mode": "shared_memory",
                "source_outputs": ["processed_data"],
                "target_inputs": ["processed_data"],
                "data_formats": ["text", "binary"],
                "supported_events": ["node_complete", "data_ready"]
            },
            {
                "id": "edge2",
                "source": "node2",
                "target": "node3",
                "transfer_mode": "shared_memory",
                "source_outputs": ["output_data"],
                "target_inputs": ["output_data"],
                "data_formats": ["text"],
                "supported_events": ["node_complete", "data_ready"]
            }
        ]
    })";

  // Parse the workflow
  Logger::instance().info("Parsing workflow from DSL...");
  auto parser = WorkflowParserFactory::create();
  auto result = parser->parse_workflow(workflow_dsl);

  // Check parsing result
  if(result.is_valid()) {
    Logger::instance().info("Workflow parsed successfully!");
    Logger::instance().info_f("Workflow ID: %s", result.workflow->get_id().c_str());
    Logger::instance().info_f("Workflow Name: %s", result.workflow->get_name().c_str());
    Logger::instance().info_f("Number of nodes: %zu", result.workflow->get_nodes().size());
    Logger::instance().info_f("Number of edges: %zu", result.workflow->get_edges().size());

    // List nodes
    for(const auto& node : result.workflow->get_nodes()) {
      Logger::instance().info_f("  Node: %s (type: %s, priority: %d)", node->get_name().c_str(),
                                node->get_type().c_str(), node->get_priority());
    }

    // Validate workflow
    Logger::instance().info("Validating workflow structure...");
    ParseResult validate_result;
    if(parser->validate_workflow(result.workflow, validate_result)) {
      Logger::instance().info("Workflow validation passed!");
    } else {
      Logger::instance().error("Workflow validation failed!");
      for(const auto& error : validate_result.errors) {
        Logger::instance().error_f("  %s", error.to_string().c_str());
      }
    }

    // Test topological sorting
    Logger::instance().info("Getting topological order...");
    auto topological_order = result.workflow->get_topological_order();
    Logger::instance().info_f("Topological order: %zu nodes", topological_order.size());
    for(size_t i = 0; i < topological_order.size(); ++i) {
      auto node = result.workflow->get_node(topological_order[i]);
      if(node) {
        Logger::instance().info_f("  %zu. %s", i + 1, node->get_name().c_str());
      }
    }

    // Test executable nodes
    Logger::instance().info("Getting executable nodes (all nodes initially READY)...");
    auto executable = result.workflow->get_executable_nodes();
    Logger::instance().info_f("Executable nodes: %zu", executable.size());
    for(const auto& node_id : executable) {
      auto node = result.workflow->get_node(node_id);
      if(node) {
        Logger::instance().info_f("  - %s", node->get_name().c_str());
      }
    }
  } else {
    Logger::instance().error("Failed to parse workflow!");
    for(const auto& error : result.errors) {
      Logger::instance().error_f("  %s", error.to_string().c_str());
    }
    return 1;
  }

  // Print warnings if any
  if(!result.warnings.empty()) {
    Logger::instance().info("Warnings:");
    for(const auto& warning : result.warnings) {
      Logger::instance().info_f("  %s", warning.c_str());
    }
  }

  Logger::instance().info("Example completed successfully!");
  return 0;
}
