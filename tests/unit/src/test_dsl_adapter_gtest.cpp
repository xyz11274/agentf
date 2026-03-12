// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Unit tests for DSL Adapter - THIRD IR to AF DSL conversion

#include <filesystem>
#include <fstream>
#include <memory>

#include <gtest/gtest.h>

#include "workflow/dsl/dsl_adapter.h"
#include "workflow/edge.h"
#include "workflow/node.h"
#include "workflow/workflow.h"
#include "agent/agent.h"

namespace agentflow {
namespace tests {

class DSLAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override { adapter_ = std::make_unique<agentflow::dsl::DSLAdapter>(); }

  std::unique_ptr<agentflow::dsl::DSLAdapter> adapter_;

  // Valid THIRD IR Workflow JSON (Component Instance format - inputs/outputs are Map)
  const char* valid_thirdir_workflow_json_ = R"thirdir({
    "schemaVersion": "0.6",
    "workflowId": "third_workflow_001",
    "workflowName": "THIRD Test Workflow",
    "workflowVersion": "1.0",
    "description": "A test workflow in THIRD IR format",
    "config": {
      "timeout": 30000,
      "retry_count": 3
    },
    "components": [
      {
        "id": "start_node",
        "type": "StartComponent",
        "typeVersion": "1.0",
        "name": "Start",
        "classInfo": {
          "programLanguage": "python",
          "modulePath": "./nodes/start_node.py",
          "className": "StartNode",
          "filePath": "./nodes/start_node.py"
        },
        "inputs": {
          "query": "user query string",
          "context": {"key": "value"}
        },
        "outputs": {
          "result": ""
        }
      },
      {
        "id": "process_node",
        "type": "ProcessComponent",
        "typeVersion": "1.0",
        "name": "Process",
        "classInfo": {
          "programLanguage": "c++",
          "modulePath": "./nodes/libprocess.so",
          "className": "ProcessNode",
          "filePath": "./nodes/libprocess.so"
        },
        "inputs": {
          "input_data": "${start_node.result}"
        },
        "outputs": {
          "output_data": ""
        }
      },
      {
        "id": "end_node",
        "type": "EndComponent",
        "typeVersion": "1.0",
        "name": "End",
        "classInfo": {
          "programLanguage": "shell",
          "filePath": "./nodes/end_node.sh"
        },
        "inputs": {
          "final_result": "${process_node.output_data}"
        },
        "outputs": {}
      }
    ],
    "connections": [
      {
        "source": {"componentId": "start_node"},
        "target": {"componentId": "process_node"}
      },
      {
        "source": {"componentId": "process_node"},
        "target": {"componentId": "end_node"}
      }
    ]
  })thirdir";

  // Valid THIRD IR Agent JSON
  const char* valid_thirdir_agent_json_ = R"thirdir({
    "schemaVersion": "0.6",
    "agentID": "third_agent_001",
    "agentVersion": "1.0",
    "description": "A test agent in THIRD IR format",
    "workflow": {
      "workflowId": "agent_workflow_001",
      "workflowName": "Agent Workflow",
      "workflowVersion": "1.0",
      "schemaVersion": "0.6",
      "components": [
        {
          "id": "input_node",
          "type": "InputComponent",
          "typeVersion": "1.0",
          "name": "Input",
          "classInfo": {
            "programLanguage": "python",
            "filePath": "./nodes/input.py"
          },
          "inputs": {},
          "outputs": {
            "data": ""
          }
        },
        {
          "id": "output_node",
          "type": "OutputComponent",
          "typeVersion": "1.0",
          "name": "Output",
          "classInfo": {
            "programLanguage": "python",
            "filePath": "./nodes/output.py"
          },
          "inputs": {
            "data": "${input_node.data}"
          },
          "outputs": {}
        }
      ],
      "connections": [
        {
          "source": {"componentId": "input_node"},
          "target": {"componentId": "output_node"}
        }
      ]
    }
  })thirdir";

  // THIRD IR with switchCase component
  const char* thirdir_switchcase_workflow_json_ = R"thirdir({
    "schemaVersion": "0.6",
    "workflowId": "switch_workflow_001",
    "workflowName": "SwitchCase Workflow",
    "workflowVersion": "1.0",
    "components": [
      {
        "id": "decision_node",
        "type": "switchCase",
        "typeVersion": "1.0",
        "name": "Decision",
        "branches": [
          {"id": "branch_a", "boolExpression": "$input.type == 'A'"},
          {"id": "branch_b", "boolExpression": "$input.type == 'B'"},
          {"id": "default", "boolExpression": ""}
        ]
      },
      {
        "id": "path_a",
        "type": "TaskComponent",
        "typeVersion": "1.0",
        "name": "Path A",
        "inputs": {},
        "outputs": {}
      },
      {
        "id": "path_b",
        "type": "TaskComponent",
        "typeVersion": "1.0",
        "name": "Path B",
        "inputs": {},
        "outputs": {}
      },
      {
        "id": "path_default",
        "type": "TaskComponent",
        "typeVersion": "1.0",
        "name": "Default Path",
        "inputs": {},
        "outputs": {}
      }
    ],
    "connections": [
      {
        "source": {"componentId": "decision_node", "branchId": "branch_a"},
        "target": {"componentId": "path_a"}
      },
      {
        "source": {"componentId": "decision_node", "branchId": "branch_b"},
        "target": {"componentId": "path_b"}
      },
      {
        "source": {"componentId": "decision_node", "branchId": "default"},
        "target": {"componentId": "path_default"}
      }
    ]
  })thirdir";

  // Invalid THIRD IR workflow (missing required fields)
  const char* invalid_thirdir_workflow_json_ = R"thirdir({
    "schemaVersion": "0.6",
    "workflowName": "Invalid Workflow",
    "components": []
  })thirdir";
};

// =============================================================================
// Format Detection Tests
// =============================================================================

TEST_F(DSLAdapterTest, DetectTHIRDIRFormat) {
  EXPECT_EQ("thirdir", agentflow::dsl::DSLAdapter::detect_format(valid_thirdir_workflow_json_));
  EXPECT_EQ("thirdir", agentflow::dsl::DSLAdapter::detect_format(valid_thirdir_agent_json_));
}

TEST_F(DSLAdapterTest, DetectAFDSLFormat) {
  const char* afdsl_json = R"thirdir({
    "id": "workflow_001",
    "name": "Test",
    "nodes": [],
    "edges": []
  })thirdir";
  EXPECT_EQ("afdsl", agentflow::dsl::DSLAdapter::detect_format(afdsl_json));
}

TEST_F(DSLAdapterTest, DetectUnknownFormat) {
  const char* unknown_json = R"thirdir({
    "foo": "bar"
  })thirdir";
  EXPECT_EQ("unknown", agentflow::dsl::DSLAdapter::detect_format(unknown_json));
}

// =============================================================================
// Workflow Conversion Tests
// =============================================================================

TEST_F(DSLAdapterTest, ConvertValidTHIRDIRWorkflow) {
  auto result = adapter_->convert_workflow(valid_thirdir_workflow_json_);

  EXPECT_TRUE(result.is_valid()) << result.get_error_summary();
  EXPECT_NE(nullptr, result.workflow);
  EXPECT_EQ("third_workflow_001", result.workflow->get_id());
  EXPECT_EQ("THIRD Test Workflow", result.workflow->get_name());
  EXPECT_EQ("1.0", result.workflow->get_config().version);
  EXPECT_EQ("A test workflow in THIRD IR format", result.workflow->get_config().description);
}

TEST_F(DSLAdapterTest, ConvertWorkflowNodes) {
  auto result = adapter_->convert_workflow(valid_thirdir_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  const auto& nodes = result.workflow->get_nodes();

  ASSERT_EQ(3, nodes.size());

  // Check start_node
  auto start_node = result.workflow->get_node("start_node");
  ASSERT_NE(nullptr, start_node);
  EXPECT_EQ("start_node", start_node->get_id());
  EXPECT_EQ("Start", start_node->get_name());
  EXPECT_EQ("StartComponent", start_node->get_type());
  EXPECT_EQ(framework::MMNode::ExecutionMode::PYTHON_SCRIPT, start_node->get_execution_mode());

  // Check process_node
  auto process_node = result.workflow->get_node("process_node");
  ASSERT_NE(nullptr, process_node);
  EXPECT_EQ("process_node", process_node->get_id());
  EXPECT_EQ("Process", process_node->get_name());
  EXPECT_EQ("ProcessComponent", process_node->get_type());
  EXPECT_EQ(framework::MMNode::ExecutionMode::SO_LIBRARY, process_node->get_execution_mode());

  // Check end_node
  auto end_node = result.workflow->get_node("end_node");
  ASSERT_NE(nullptr, end_node);
  EXPECT_EQ("end_node", end_node->get_id());
  EXPECT_EQ("End", end_node->get_name());
  EXPECT_EQ(framework::MMNode::ExecutionMode::SHELL_SCRIPT, end_node->get_execution_mode());
}

TEST_F(DSLAdapterTest, ConvertWorkflowEdges) {
  auto result = adapter_->convert_workflow(valid_thirdir_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  const auto& edges = result.workflow->get_edges();

  ASSERT_EQ(2, edges.size());

  // Check first edge
  EXPECT_EQ("start_node", edges[0]->get_source());
  EXPECT_EQ("process_node", edges[0]->get_target());

  // Check second edge
  EXPECT_EQ("process_node", edges[1]->get_source());
  EXPECT_EQ("end_node", edges[1]->get_target());
}

TEST_F(DSLAdapterTest, ConvertNodeInputsOutputs) {
  auto result = adapter_->convert_workflow(valid_thirdir_workflow_json_);

  ASSERT_TRUE(result.is_valid());

  auto start_node = result.workflow->get_node("start_node");
  ASSERT_NE(nullptr, start_node);

  const auto& input_defs = start_node->get_input_definitions();
  EXPECT_EQ(2, input_defs.size());
  EXPECT_EQ("query", input_defs[0].name);
  EXPECT_EQ("context", input_defs[1].name);

  const auto& output_defs = start_node->get_output_definitions();
  EXPECT_EQ(1, output_defs.size());
  EXPECT_EQ("result", output_defs[0].name);
}

TEST_F(DSLAdapterTest, ConvertSwitchCaseWorkflow) {
  auto result = adapter_->convert_workflow(thirdir_switchcase_workflow_json_);

  EXPECT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(nullptr, result.workflow);

  const auto& edges = result.workflow->get_edges();
  ASSERT_EQ(3, edges.size());

  // Check branch edges have switch-case control type
  for(const auto& edge : edges) {
    EXPECT_TRUE(edge->has_control());
    EXPECT_EQ(framework::MMEdge::ControlType::SWITCH_CASE, edge->get_control_type());
  }

  // Check default branch
  bool found_default = false;
  for(const auto& edge : edges) {
    if(edge->get_control_config().is_default) {
      found_default = true;
      break;
    }
  }
  EXPECT_TRUE(found_default);
}

// =============================================================================
// Agent Conversion Tests
// =============================================================================

TEST_F(DSLAdapterTest, ConvertValidTHIRDIRAgent) {
  auto result = adapter_->convert_agent(valid_thirdir_agent_json_);

  EXPECT_TRUE(result.is_valid()) << result.get_error_summary();
  EXPECT_NE(nullptr, result.agent);
  EXPECT_EQ("third_agent_001", result.agent->get_id());
  EXPECT_EQ("third_agent_001", result.agent->get_name());
  EXPECT_EQ("1.0", result.agent->get_config().version);
  EXPECT_EQ("A test agent in THIRD IR format", result.agent->get_config().description);
}

TEST_F(DSLAdapterTest, ConvertAgentWithWorkflow) {
  auto result = adapter_->convert_agent(valid_thirdir_agent_json_);

  ASSERT_TRUE(result.is_valid());

  const auto& workflows = result.agent->get_workflows();
  ASSERT_EQ(1, workflows.size());

  EXPECT_EQ("agent_workflow_001", workflows[0]->get_id());
  EXPECT_EQ("Agent Workflow", workflows[0]->get_name());
}

// =============================================================================
// JSON Output Tests
// =============================================================================

TEST_F(DSLAdapterTest, ConvertWorkflowToJSON) {
  auto result = adapter_->convert_workflow_to_json(valid_thirdir_workflow_json_);

  EXPECT_TRUE(result.is_valid()) << result.get_error_summary();
  EXPECT_FALSE(result.workflow_json.empty());

  // Check that output JSON contains expected fields
  EXPECT_TRUE(result.workflow_json.find("\"id\": \"third_workflow_001\"") != std::string::npos);
  EXPECT_TRUE(result.workflow_json.find("\"name\": \"THIRD Test Workflow\"") != std::string::npos);
  EXPECT_TRUE(result.workflow_json.find("\"nodes\":") != std::string::npos);
  EXPECT_TRUE(result.workflow_json.find("\"edges\":") != std::string::npos);
}

TEST_F(DSLAdapterTest, ConvertAgentToJSON) {
  auto result = adapter_->convert_agent_to_json(valid_thirdir_agent_json_);

  EXPECT_TRUE(result.is_valid()) << result.get_error_summary();
  EXPECT_FALSE(result.agent_json.empty());

  // Check that output JSON contains expected fields
  EXPECT_TRUE(result.agent_json.find("\"id\": \"third_agent_001\"") != std::string::npos);
  EXPECT_TRUE(result.agent_json.find("\"name\": \"third_agent_001\"") != std::string::npos);
  EXPECT_TRUE(result.agent_json.find("\"workflows\":") != std::string::npos);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(DSLAdapterTest, ConvertInvalidWorkflow) {
  auto result = adapter_->convert_workflow(invalid_thirdir_workflow_json_);

  EXPECT_FALSE(result.is_valid());
  EXPECT_FALSE(result.errors.empty());
}

TEST_F(DSLAdapterTest, ConvertInvalidJSON) {
  auto result = adapter_->convert_workflow("not a valid json");

  EXPECT_FALSE(result.is_valid());
  EXPECT_FALSE(result.errors.empty());
}

TEST_F(DSLAdapterTest, ConvertEmptyJSON) {
  auto result = adapter_->convert_workflow("{}");

  EXPECT_FALSE(result.is_valid());
  EXPECT_FALSE(result.errors.empty());
}

// =============================================================================
// Validation Tests
// =============================================================================

TEST_F(DSLAdapterTest, ValidateTHIRDIRContent) {
  agentflow::dsl::AdapterResult result;
  bool valid = adapter_->validate_thirdir(valid_thirdir_workflow_json_, result);

  EXPECT_TRUE(valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST_F(DSLAdapterTest, ValidateTHIRDIRContentInvalid) {
  agentflow::dsl::AdapterResult result;
  bool valid = adapter_->validate_thirdir("invalid json", result);

  EXPECT_FALSE(valid);
  EXPECT_FALSE(result.errors.empty());
}

// =============================================================================
// File-based Conversion Tests
// =============================================================================

TEST_F(DSLAdapterTest, ConvertWorkflowFromFile) {
  // Create a temporary file with THIRD IR content
  namespace fs = std::filesystem;
  fs::path temp_dir = fs::temp_directory_path();
  fs::path temp_file = temp_dir / "test_thirdir_workflow.json";

  std::ofstream ofs(temp_file);
  ofs << valid_thirdir_workflow_json_;
  ofs.close();

  auto result = adapter_->convert_workflow_file(temp_file.string());

  EXPECT_TRUE(result.is_valid()) << result.get_error_summary();
  EXPECT_NE(nullptr, result.workflow);
  EXPECT_EQ("third_workflow_001", result.workflow->get_id());

  // Cleanup
  fs::remove(temp_file);
}

TEST_F(DSLAdapterTest, ConvertAgentFromFile) {
  namespace fs = std::filesystem;
  fs::path temp_dir = fs::temp_directory_path();
  fs::path temp_file = temp_dir / "test_thirdir_agent.json";

  std::ofstream ofs(temp_file);
  ofs << valid_thirdir_agent_json_;
  ofs.close();

  auto result = adapter_->convert_agent_file(temp_file.string());

  EXPECT_TRUE(result.is_valid()) << result.get_error_summary();
  EXPECT_NE(nullptr, result.agent);
  EXPECT_EQ("third_agent_001", result.agent->get_id());

  // Cleanup
  fs::remove(temp_file);
}

// =============================================================================
// Execution Mode Mapping Tests
// =============================================================================

TEST_F(DSLAdapterTest, MapExecutionModes) {
  // Test Python execution mode
  const char* python_workflow = R"thirdir({
    "workflowId": "python_test",
    "workflowName": "Python Test",
    "workflowVersion": "1.0",
    "components": [
      {
        "id": "py_node",
        "type": "PythonComponent",
        "inputs": {},
        "outputs": {},
        "classInfo": {"programLanguage": "python", "filePath": "test.py"}
      }
    ],
    "connections": []
  })thirdir";

  auto result = adapter_->convert_workflow(python_workflow);
  ASSERT_TRUE(result.is_valid());
  auto node = result.workflow->get_node("py_node");
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(framework::MMNode::ExecutionMode::PYTHON_SCRIPT, node->get_execution_mode());
}

TEST_F(DSLAdapterTest, MapExecutionModesGo) {
  const char* go_workflow = R"thirdir({
    "workflowId": "go_test",
    "workflowName": "Go Test",
    "workflowVersion": "1.0",
    "components": [
      {
        "id": "go_node",
        "type": "GoComponent",
        "inputs": {},
        "outputs": {},
        "classInfo": {"programLanguage": "go", "filePath": "test.go"}
      }
    ],
    "connections": []
  })thirdir";

  auto result = adapter_->convert_workflow(go_workflow);
  ASSERT_TRUE(result.is_valid());
  auto node = result.workflow->get_node("go_node");
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(framework::MMNode::ExecutionMode::GO_SCRIPT, node->get_execution_mode());
}

TEST_F(DSLAdapterTest, MapExecutionModesJava) {
  const char* java_workflow = R"thirdir({
    "workflowId": "java_test",
    "workflowName": "Java Test",
    "workflowVersion": "1.0",
    "components": [
      {
        "id": "java_node",
        "type": "JavaComponent",
        "inputs": {},
        "outputs": {},
        "classInfo": {"programLanguage": "java", "className": "com.example.TestNode"}
      }
    ],
    "connections": []
  })thirdir";

  auto result = adapter_->convert_workflow(java_workflow);
  ASSERT_TRUE(result.is_valid());
  auto node = result.workflow->get_node("java_node");
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(framework::MMNode::ExecutionMode::JAVA_JAR, node->get_execution_mode());
}

// =============================================================================
// Factory Tests
// =============================================================================

TEST_F(DSLAdapterTest, FactoryCreate) {
  auto adapter = agentflow::dsl::DSLAdapterFactory::create();
  EXPECT_NE(nullptr, adapter);
}

TEST_F(DSLAdapterTest, FactoryVersions) {
  std::string thirdir_version = agentflow::dsl::DSLAdapterFactory::get_supported_thirdir_version();
  std::string afdsl_version = agentflow::dsl::DSLAdapterFactory::get_output_afdsl_version();

  EXPECT_FALSE(thirdir_version.empty());
  EXPECT_FALSE(afdsl_version.empty());
}

}  // namespace tests
}  // namespace agentflow
