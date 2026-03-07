// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <filesystem>
#include <fstream>
#include <memory>

#include <gtest/gtest.h>

#include "workflow/dsl/workflow_parser.h"
#include "workflow/edge.h"
#include "workflow/node.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace tests {

namespace {
std::filesystem::path repo_root() {
  namespace fs = std::filesystem;
  const fs::path cwd = fs::current_path();
  const std::vector<fs::path> candidates = {cwd, cwd / "..", cwd / "../.."};
  for(const auto& c : candidates) {
    if(fs::exists(c / "publish/workflow/README.md")) {
      return fs::absolute(c);
    }
  }
  return fs::absolute(cwd);
}
}  // namespace

class WorkflowJsonTest : public ::testing::Test {
 protected:
  void SetUp() override { parser_ = std::make_unique<agentflow::dsl::WorkflowParser>(); }

  std::unique_ptr<agentflow::dsl::WorkflowParser> parser_;

  const char* valid_workflow_json_ = R"({
    "id": "workflow_001",
    "name": "Typed Workflow",
    "version": "1.0.0",
    "description": "Workflow with typed node IO",
    "nodes": [
      {
        "id": "node_start",
        "name": "Start Node",
        "type": "TASK",
        "inputs": [
          {"name": "request_id", "type": "string", "required": true},
          {"name": "flags", "type": "list", "transfer_mode": "ref"}
        ],
        "outputs": [
          {"name": "status", "type": "string"},
          {"name": "payload", "type": "map", "transfer_mode": "ref"}
        ],
        "config": {
          "timeout_ms": 5000,
          "max_retries": 3,
          "retry_delay_ms": 1000,
          "execution_mode": "cpp_function"
        }
      },
      {
        "id": "node_end",
        "name": "End Node",
        "type": "TASK",
        "inputs": [
          {"name": "status", "type": "string"},
          {"name": "payload", "type": "map", "transfer_mode": "ref"}
        ],
        "outputs": [
          {"name": "final_status", "type": "string"}
        ]
      }
    ],
    "edges": [
      {
        "id": "edge_start_to_end",
        "source": "node_start",
        "target": "node_end",
        "source_outputs": ["status", "payload"],
        "target_inputs": ["status", "payload"],
        "transfer_mode": "ref",
        "control": {"type": "if", "expression": "status == 'ok'"}
      }
    ]
  })";

  const char* switch_case_workflow_json_ = R"({
    "id": "workflow_switch_001",
    "name": "Switch Case Workflow",
    "version": "1.0.0",
    "nodes": [
      {"id": "selector", "name": "Selector", "type": "TASK", "outputs": [{"name": "route", "type": "string"}]},
      {"id": "path_a", "name": "Path A", "type": "TASK"},
      {"id": "path_b", "name": "Path B", "type": "TASK"},
      {"id": "path_default", "name": "Path Default", "type": "TASK"}
    ],
    "edges": [
      {
        "id": "selector_a",
        "source": "selector",
        "target": "path_a",
        "control": {"type": "switch-case", "expression": "route", "case_value": "A"}
      },
      {
        "id": "selector_b",
        "source": "selector",
        "target": "path_b",
        "control": {"type": "switch-case", "expression": "route", "case_value": "B"}
      },
      {
        "id": "selector_default",
        "source": "selector",
        "target": "path_default",
        "control": {"type": "switch-case", "expression": "route", "is_default": true}
      }
    ]
  })";

  const char* legacy_control_workflow_json_ = R"({
    "id": "workflow_legacy_edge_001",
    "name": "Legacy Control Workflow",
    "version": "1.0.0",
    "nodes": [
      {"id": "n1", "name": "N1", "type": "TASK"},
      {"id": "n2", "name": "N2", "type": "TASK"}
    ],
    "edges": [
      {
        "id": "legacy_edge",
        "source": "n1",
        "target": "n2",
        "control": {"type": "goto", "goto_target": "n2"}
      }
    ]
  })";

  const char* legacy_object_io_workflow_json_ = R"({
    "id": "workflow_object_io_001",
    "name": "Object IO Workflow",
    "version": "1.0.0",
    "nodes": [
      {
        "id": "mapper",
        "name": "Mapper",
        "type": "TASK",
        "inputs": {
          "count": "int",
          "enabled": {"type": "bool", "required": true},
          "attrs": {"type": "map", "transfer_mode": "ref"}
        },
        "outputs": {
          "result": "float"
        }
      }
    ],
    "edges": []
  })";

  const char* invalid_workflow_circular_json_ = R"({
    "id": "workflow_circular",
    "name": "Circular Workflow",
    "version": "1.0.0",
    "nodes": [
      {"id": "node_a", "name": "A", "type": "TASK"},
      {"id": "node_b", "name": "B", "type": "TASK"},
      {"id": "node_c", "name": "C", "type": "TASK"}
    ],
    "edges": [
      {"id": "edge_a_b", "source": "node_a", "target": "node_b"},
      {"id": "edge_b_c", "source": "node_b", "target": "node_c"},
      {"id": "edge_c_a", "source": "node_c", "target": "node_a"}
    ]
  })";
};

TEST_F(WorkflowJsonTest, ParseValidWorkflowWithTypedIo) {
  auto result = parser_->parse_workflow(valid_workflow_json_);
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(result.workflow, nullptr);
  ASSERT_EQ(result.workflow->get_nodes().size(), 2u);
  ASSERT_EQ(result.workflow->get_edges().size(), 1u);

  auto start_node = result.workflow->get_node("node_start");
  ASSERT_NE(start_node, nullptr);
  ASSERT_EQ(start_node->get_input_definitions().size(), 2u);
  ASSERT_EQ(start_node->get_output_definitions().size(), 2u);
  EXPECT_EQ(start_node->get_input_definitions()[0].type, framework::MMNode::FieldType::STRING);
  EXPECT_EQ(start_node->get_input_definitions()[1].type, framework::MMNode::FieldType::LIST);
  EXPECT_EQ(start_node->get_input_definitions()[1].transfer_mode,
            framework::MMNode::TransferMode::REFERENCE);
  EXPECT_EQ(start_node->get_output_definitions()[1].type, framework::MMNode::FieldType::MAP);

  const auto& edge = result.workflow->get_edges().front();
  EXPECT_EQ(edge->get_transfer_mode(), framework::MMEdge::DataTransferMode::REFERENCE);
  EXPECT_EQ(edge->get_control_type(), framework::MMEdge::ControlType::IF);
  EXPECT_EQ(edge->get_condition_expression(), "status == 'ok'");
}

TEST_F(WorkflowJsonTest, ParseObjectStyleIoAsTypedDefinitions) {
  auto result = parser_->parse_workflow(legacy_object_io_workflow_json_);
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  auto node = result.workflow->get_node("mapper");
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(node->get_input_definitions().size(), 3u);
  EXPECT_EQ(node->get_input_definitions()[0].name, "count");
  EXPECT_EQ(node->get_input_definitions()[0].type, framework::MMNode::FieldType::INT);
  EXPECT_EQ(node->get_input_definitions()[1].type, framework::MMNode::FieldType::BOOL);
  EXPECT_TRUE(node->get_input_definitions()[1].required);
  EXPECT_EQ(node->get_input_definitions()[2].transfer_mode,
            framework::MMNode::TransferMode::REFERENCE);
  ASSERT_EQ(node->get_output_definitions().size(), 1u);
  EXPECT_EQ(node->get_output_definitions()[0].type, framework::MMNode::FieldType::FLOAT);
}

TEST_F(WorkflowJsonTest, ParseSwitchCaseWorkflow) {
  auto result = parser_->parse_workflow(switch_case_workflow_json_);
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(result.workflow, nullptr);
  ASSERT_EQ(result.workflow->get_edges().size(), 3u);
  for(const auto& edge : result.workflow->get_edges()) {
    EXPECT_EQ(edge->get_control_type(), framework::MMEdge::ControlType::SWITCH_CASE);
    EXPECT_EQ(edge->get_control_config().expression, "route");
  }
  EXPECT_TRUE(result.workflow->validate());
}

TEST_F(WorkflowJsonTest, SwitchCaseSelectsMatchingAndDefaultBranches) {
  auto result = parser_->parse_workflow(switch_case_workflow_json_);
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_TRUE(result.workflow->validate());

  auto selector = result.workflow->get_node("selector");
  auto path_a = result.workflow->get_node("path_a");
  auto path_b = result.workflow->get_node("path_b");
  auto path_default = result.workflow->get_node("path_default");
  ASSERT_NE(selector, nullptr);
  ASSERT_NE(path_a, nullptr);
  ASSERT_NE(path_b, nullptr);
  ASSERT_NE(path_default, nullptr);

  path_a->set_state(NodeState::READY);
  path_b->set_state(NodeState::READY);
  path_default->set_state(NodeState::READY);
  selector->set_state(NodeState::SUCCESS);
  selector->set_outputs({{"route", std::string("B")}});
  result.workflow->mark_node_executed("selector", true);
  auto executable = result.workflow->get_executable_nodes_with_control();
  EXPECT_NE(std::find(executable.begin(), executable.end(), "path_b"), executable.end());
  EXPECT_EQ(std::find(executable.begin(), executable.end(), "path_a"), executable.end());
  EXPECT_EQ(std::find(executable.begin(), executable.end(), "path_default"), executable.end());

  selector->set_outputs({{"route", std::string("Z")}});
  executable = result.workflow->get_executable_nodes_with_control();
  EXPECT_NE(std::find(executable.begin(), executable.end(), "path_default"), executable.end());
}

TEST_F(WorkflowJsonTest, RejectLegacyEdgeControlTypes) {
  auto result = parser_->parse_workflow(legacy_control_workflow_json_);
  EXPECT_FALSE(result.is_valid());
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.get_error_summary().find("no longer supported"), std::string::npos);
}

TEST_F(WorkflowJsonTest, ValidateWorkflowGraphStructure) {
  auto result = parser_->parse_workflow(valid_workflow_json_);
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  EXPECT_TRUE(result.workflow->validate());
}

TEST_F(WorkflowJsonTest, CircularWorkflowValidationFails) {
  auto result = parser_->parse_workflow(invalid_workflow_circular_json_);
  EXPECT_FALSE(result.is_valid());
  EXPECT_NE(result.workflow, nullptr);
  EXPECT_NE(result.get_error_summary().find("acyclic"), std::string::npos);
}

TEST_F(WorkflowJsonTest, ReferenceTransferProducesDatabusRefMarker) {
  framework::MMEdge::Config config;
  config.id = "edge_ref";
  config.source_node = "source";
  config.target_node = "target";
  config.source_outputs = {"payload"};
  config.target_inputs = {"payload_ref"};
  config.transfer_mode = framework::MMEdge::DataTransferMode::REFERENCE;
  framework::MMEdge edge(config);

  DataMap source_data;
  source_data["payload"] = std::string("body");
  DataMap target_data;
  ASSERT_TRUE(edge.transfer_data(source_data, target_data));
  ASSERT_TRUE(target_data.count("payload_ref") > 0);
  EXPECT_EQ(std::get<std::string>(target_data["payload_ref"]), "databus://global/source.payload");
}

TEST_F(WorkflowJsonTest, ParseUpdatedPatternWorkflowFile) {
  namespace fs = std::filesystem;
  fs::path pattern = repo_root() / "publish/workflow/pattern/dynamic_workflow.json";
  ASSERT_TRUE(fs::exists(pattern));

  auto result = parser_->parse_workflow_file(pattern.string());
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(result.workflow, nullptr);
  EXPECT_NE(result.workflow->get_node("dynamic_workflow"), nullptr);
}

}  // namespace tests
}  // namespace agentflow
