// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example: DSLAdapter Usage - Converting THIRD IR to AF DSL format
//
// This example demonstrates how to use the DSLAdapter module to convert
// Agent definitions from THIRD IR format to AgentFlow DSL format.

#include <iostream>
#include <memory>
#include <string>

#include "workflow/dsl/dsl_adapter.h"
#include "workflow/workflow.h"
#include "agent/agent.h"

using namespace agentflow;

// Example THIRD IR Agent JSON (as would be loaded from a file)
const char* EXAMPLE_THIRDIR_AGENT = R"thirdir({
  "schemaVersion": "0.6",
  "agentID": "MathAgent",
  "agentVersion": "1.0.0",
  "description": "A simple math agent that performs basic calculations",
  "workflow": {
    "workflowId": "math_workflow",
    "workflowName": "Math Calculation Workflow",
    "workflowVersion": "1.0",
    "description": "Workflow for performing math operations",
    "components": [
      {
        "id": "input_parser",
        "type": "InputParser",
        "typeVersion": "1.0",
        "name": "Input Parser",
        "classInfo": {
          "programLanguage": "python",
          "modulePath": "./components/input_parser.py",
          "className": "InputParser",
          "filePath": "./components/input_parser.py"
        },
        "inputs": [
          {"id": "raw_input", "type": "String", "description": "Raw user input"}
        ],
        "outputs": [
          {"id": "operation", "type": "String", "description": "Operation type"},
          {"id": "operand1", "type": "Float", "description": "First operand"},
          {"id": "operand2", "type": "Float", "description": "Second operand"}
        ]
      },
      {
        "id": "calculator",
        "type": "Calculator",
        "typeVersion": "1.0",
        "name": "Calculator",
        "classInfo": {
          "programLanguage": "c++",
          "modulePath": "./components/libcalculator.so",
          "className": "Calculator",
          "filePath": "./components/libcalculator.so"
        },
        "inputs": [
          {"id": "operation", "type": "String"},
          {"id": "operand1", "type": "Float"},
          {"id": "operand2", "type": "Float"}
        ],
        "outputs": [
          {"id": "result", "type": "Float", "description": "Calculation result"}
        ]
      },
      {
        "id": "result_formatter",
        "type": "ResultFormatter",
        "typeVersion": "1.0",
        "name": "Result Formatter",
        "classInfo": {
          "programLanguage": "shell",
          "filePath": "./components/format_result.sh"
        },
        "inputs": [
          {"id": "result", "type": "Float"},
          {"id": "operation", "type": "String"}
        ],
        "outputs": [
          {"id": "formatted_output", "type": "String", "description": "Formatted result string"}
        ]
      }
    ],
    "connections": [
      {
        "source": {"componentId": "input_parser"},
        "target": {"componentId": "calculator"}
      },
      {
        "source": {"componentId": "calculator"},
        "target": {"componentId": "result_formatter"}
      }
    ]
  }
})thirdir";

// Example THIRD IR Workflow with switch-case (conditional branches)
const char* EXAMPLE_THIRDIR_WORKFLOW_SWITCH = R"thirdir({
  "schemaVersion": "0.6",
  "workflowId": "conditional_workflow",
  "workflowName": "Conditional Processing Workflow",
  "workflowVersion": "1.0",
  "description": "Workflow with conditional branching",
  "components": [
    {
      "id": "router",
      "type": "switchCase",
      "typeVersion": "1.0",
      "name": "Request Router",
      "branches": [
        {"id": "fast_path", "boolExpression": "$request.priority == 'high'"},
        {"id": "normal_path", "boolExpression": "$request.priority == 'normal'"},
        {"id": "default", "boolExpression": ""}
      ]
    },
    {
      "id": "fast_processor",
      "type": "FastProcessor",
      "typeVersion": "1.0",
      "name": "Fast Processor",
      "classInfo": {
        "programLanguage": "go",
        "filePath": "./processors/fast_processor.go"
      }
    },
    {
      "id": "normal_processor",
      "type": "NormalProcessor",
      "typeVersion": "1.0",
      "name": "Normal Processor",
      "classInfo": {
        "programLanguage": "python",
        "filePath": "./processors/normal_processor.py"
      }
    },
    {
      "id": "fallback_processor",
      "type": "FallbackProcessor",
      "typeVersion": "1.0",
      "name": "Fallback Processor",
      "classInfo": {
        "programLanguage": "shell",
        "filePath": "./processors/fallback.sh"
      }
    },
    {
      "id": "output_node",
      "type": "OutputHandler",
      "typeVersion": "1.0",
      "name": "Output Handler",
      "classInfo": {
        "programLanguage": "python",
        "filePath": "./handlers/output.py"
      },
      "inputs": [
        {"id": "processed_data", "type": "String"}
      ],
      "outputs": [
        {"id": "response", "type": "String"}
      ]
    }
  ],
  "connections": [
    {
      "source": {"componentId": "router", "branchId": "fast_path"},
      "target": {"componentId": "fast_processor"}
    },
    {
      "source": {"componentId": "router", "branchId": "normal_path"},
      "target": {"componentId": "normal_processor"}
    },
    {
      "source": {"componentId": "router", "branchId": "default"},
      "target": {"componentId": "fallback_processor"}
    },
    {
      "source": {"componentId": "fast_processor"},
      "target": {"componentId": "output_node"}
    },
    {
      "source": {"componentId": "normal_processor"},
      "target": {"componentId": "output_node"}
    },
    {
      "source": {"componentId": "fallback_processor"},
      "target": {"componentId": "output_node"}
    }
  ]
})thirdir";

void printSeparator(const std::string& title) {
  std::cout << "\n" << std::string(60, '=') << "\n";
  std::cout << title << "\n";
  std::cout << std::string(60, '=') << "\n\n";
}

void demonstrateBasicConversion() {
  printSeparator("1. Basic THIRD IR Agent Conversion");

  // Create adapter
  auto adapter = dsl::DSLAdapterFactory::create();

  std::cout << "Converting THIRD IR Agent to AF DSL format...\n\n";

  // Convert agent
  auto result = adapter->convert_agent(EXAMPLE_THIRDIR_AGENT);

  if(!result.is_valid()) {
    std::cerr << "Conversion failed:\n" << result.get_error_summary() << "\n";
    return;
  }

  std::cout << "Conversion successful!\n\n";

  // Print agent information
  auto agent = result.agent;
  std::cout << "Agent ID: " << agent->get_id() << "\n";
  std::cout << "Agent Name: " << agent->get_name() << "\n";
  std::cout << "Agent Version: " << agent->get_config().version << "\n";
  std::cout << "Description: " << agent->get_config().description << "\n\n";

  // Print workflow information
  const auto& workflows = agent->get_workflows();
  std::cout << "Number of workflows: " << workflows.size() << "\n";

  for(const auto& workflow : workflows) {
    std::cout << "\n  Workflow: " << workflow->get_name() << " (ID: " << workflow->get_id() << ")\n";
    std::cout << "  Nodes: " << workflow->get_nodes().size() << "\n";
    std::cout << "  Edges: " << workflow->get_edges().size() << "\n";

    // Print nodes
    std::cout << "\n  Nodes:\n";
    for(const auto& node : workflow->get_nodes()) {
      std::cout << "    - " << node->get_name() << " (ID: " << node->get_id()
                << ", Type: " << node->get_type() << ")\n";

      // Print inputs
      const auto& inputs = node->get_input_definitions();
      if(!inputs.empty()) {
        std::cout << "      Inputs: ";
        for(size_t i = 0; i < inputs.size(); ++i) {
          if(i > 0) std::cout << ", ";
          std::cout << inputs[i].name;
        }
        std::cout << "\n";
      }

      // Print outputs
      const auto& outputs = node->get_output_definitions();
      if(!outputs.empty()) {
        std::cout << "      Outputs: ";
        for(size_t i = 0; i < outputs.size(); ++i) {
          if(i > 0) std::cout << ", ";
          std::cout << outputs[i].name;
        }
        std::cout << "\n";
      }
    }
  }
}

void demonstrateJSONOutput() {
  printSeparator("2. JSON Output Generation");

  auto adapter = dsl::DSLAdapterFactory::create();

  std::cout << "Converting THIRD IR to AF DSL JSON format...\n\n";

  // Convert and get JSON output
  auto result = adapter->convert_agent_to_json(EXAMPLE_THIRDIR_AGENT);

  if(!result.is_valid()) {
    std::cerr << "Conversion failed:\n" << result.get_error_summary() << "\n";
    return;
  }

  std::cout << "Generated AF DSL JSON:\n\n";
  std::cout << result.agent_json << "\n";
}

void demonstrateSwitchCaseConversion() {
  printSeparator("3. Switch-Case Workflow Conversion");

  auto adapter = dsl::DSLAdapterFactory::create();

  std::cout << "Converting THIRD IR Workflow with switch-case components...\n\n";

  auto result = adapter->convert_workflow(EXAMPLE_THIRDIR_WORKFLOW_SWITCH);

  if(!result.is_valid()) {
    std::cerr << "Conversion failed:\n" << result.get_error_summary() << "\n";
    return;
  }

  auto workflow = result.workflow;
  std::cout << "Workflow: " << workflow->get_name() << "\n\n";

  // Print edges with branch information
  std::cout << "Edges (with branch conditions):\n";
  for(const auto& edge : workflow->get_edges()) {
    std::cout << "  - " << edge->get_source() << " -> " << edge->get_target();

    if(edge->has_control()) {
      std::cout << " [" << edge->get_control_config().case_value;
      if(edge->get_control_config().is_default) {
        std::cout << " (default)";
      }
      std::cout << "]";
    }
    std::cout << "\n";
  }
}

void demonstrateFormatDetection() {
  printSeparator("4. Format Detection");

  std::cout << "Detecting input format...\n\n";

  // Detect THIRD IR format
  std::string thirdir_format = dsl::DSLAdapter::detect_format(EXAMPLE_THIRDIR_AGENT);
  std::cout << "EXAMPLE_THIRDIR_AGENT format: " << thirdir_format << "\n";

  // Detect AF DSL format
  const char* afdsl_json = R"thirdir({
    "id": "test_workflow",
    "name": "Test",
    "nodes": [],
    "edges": []
  })thirdir";
  std::string afdsl_format = dsl::DSLAdapter::detect_format(afdsl_json);
  std::cout << "AF DSL JSON format: " << afdsl_format << "\n";

  // Unknown format
  const char* unknown_json = R"thirdir({"foo": "bar"})thirdir";
  std::string unknown_format = dsl::DSLAdapter::detect_format(unknown_json);
  std::cout << "Unknown JSON format: " << unknown_format << "\n";
}

void demonstrateErrorHandling() {
  printSeparator("5. Error Handling");

  auto adapter = dsl::DSLAdapterFactory::create();

  // Try to convert invalid JSON
  std::cout << "Attempting to convert invalid JSON...\n\n";

  const char* invalid_json = R"thirdir({
    "schemaVersion": "0.6",
    "workflowName": "Invalid Workflow"
  })thirdir";

  auto result = adapter->convert_workflow(invalid_json);

  if(!result.is_valid()) {
    std::cout << "Conversion failed as expected:\n";
    std::cout << result.get_error_summary() << "\n";
  }
}

void demonstrateFileConversion() {
  printSeparator("6. File-based Conversion Example");

  std::cout << "To convert from a file:\n\n";
  std::cout << "  auto adapter = dsl::DSLAdapterFactory::create();\n";
  std::cout << "  auto result = adapter->convert_agent_file(\"path/to/third_agent.json\");\n";
  std::cout << "  if (result.is_valid()) {\n";
  std::cout << "      auto agent = result.agent;\n";
  std::cout << "      // Use the converted agent...\n";
  std::cout << "  }\n\n";

  std::cout << "Supported file formats:\n";
  std::cout << "  - THIRD IR Agent JSON (MathAgent_3RD_1.0.0.tar.gz -> MathAgentIR.json)\n";
  std::cout << "  - THIRD IR Workflow JSON (workflow_XX.json)\n";
}

int main() {
  std::cout << R"(
================================================================================
                       DSLAdapter Usage Example
           Converting THIRD IR format to AgentFlow DSL format
================================================================================

This example demonstrates how to use the DSLAdapter module to convert
Agent definitions from THIRD IR format to AgentFlow DSL format.

THIRD IR Format Features:
  - Uses 'components' and 'connections' concepts
  - Components have type, typeVersion, classInfo, inputs, outputs
  - Connections have source (componentId, branchId) and target
  - Supports switchCase for conditional branching

AF DSL Format Features:
  - Uses 'nodes' and 'edges' concepts
  - Nodes have type, execution_mode, command, input_params, output_params
  - Edges have source, target, condition_expression, control
  - Supports if and switch-case for conditional execution

)" << "\n";

  // Run demonstrations
  demonstrateBasicConversion();
  demonstrateJSONOutput();
  demonstrateSwitchCaseConversion();
  demonstrateFormatDetection();
  demonstrateErrorHandling();
  demonstrateFileConversion();

  printSeparator("Summary");

  std::cout << "Key DSLAdapter APIs:\n\n";
  std::cout << "  // Convert THIRD IR to objects\n";
  std::cout << "  convert_workflow(thirdir_json) -> AdapterResult { workflow }\n";
  std::cout << "  convert_agent(thirdir_json) -> AdapterResult { agent }\n\n";
  std::cout << "  // Convert THIRD IR to AF DSL JSON\n";
  std::cout << "  convert_workflow_to_json(thirdir_json) -> AdapterResult { workflow_json }\n";
  std::cout << "  convert_agent_to_json(thirdir_json) -> AdapterResult { agent_json }\n\n";
  std::cout << "  // File-based conversion\n";
  std::cout << "  convert_workflow_file(path) -> AdapterResult { workflow }\n";
  std::cout << "  convert_agent_file(path) -> AdapterResult { agent }\n\n";
  std::cout << "  // Format detection\n";
  std::cout << "  detect_format(json) -> \"thirdir\" | \"afdsl\" | \"unknown\"\n";

  std::cout << "\nExample completed successfully!\n";

  return 0;
}
