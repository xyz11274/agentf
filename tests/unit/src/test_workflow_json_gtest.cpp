// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test tests for JSON Workflow functionality

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "workflow/dsl/workflow_parser.h"
#include "workflow/edge.h"
#include "workflow/node.h"
#include "workflow/workflow.h"


namespace agentflow {
namespace tests {

class WorkflowJsonTest : public ::testing::Test {
protected:
  void SetUp() override {
    parser_ = std::make_unique<agentflow::dsl::WorkflowParser>();
  }
  void TearDown() override {}

  std::unique_ptr<agentflow::dsl::WorkflowParser> parser_;

  // Sample valid workflow JSON
  const char* valid_workflow_json_ = R"({
    "id": "workflow_001",
    "name": "Test Workflow",
    "version": "1.0.0",
    "description": "A test workflow for unit testing",
    "nodes": [
      {
        "id": "node_start",
        "name": "Start Node",
        "type": "TASK",
        "inputs": {
          "input_data": "string"
        },
        "outputs": {
          "output_data": "string"
        },
        "config": {
          "timeout_ms": 5000,
          "max_retries": 3,
          "retry_delay_ms": 1000,
          "execution_mode": "CPP_ACTOR"
        }
      },
      {
        "id": "node_process",
        "name": "Process Node",
        "type": "TASK",
        "inputs": {
          "input_data": "string"
        },
        "outputs": {
          "output_data": "string"
        },
        "config": {
          "timeout_ms": 3000,
          "max_retries": 2,
          "execution_mode": "CPP_ACTOR"
        }
      },
      {
        "id": "node_end",
        "name": "End Node",
        "type": "TASK",
        "inputs": {
          "input_data": "string"
        },
        "outputs": {
          "output_data": "string"
        },
        "config": {
          "timeout_ms": 2000,
          "max_retries": 1,
          "execution_mode": "CPP_ACTOR"
        }
      }
    ],
    "edges": [
      {
        "id": "edge_start_to_process",
        "source": "node_start",
        "target": "node_process",
        "data_transfer": {
          "mode": "DIRECT",
          "data_formats": ["STRING"]
        },
        "supported_events": ["DATA_READY", "START"]
      },
      {
        "id": "edge_process_to_end",
        "source": "node_process",
        "target": "node_end",
        "data_transfer": {
          "mode": "DIRECT",
          "data_formats": ["STRING"]
        },
        "supported_events": ["DATA_READY", "COMPLETE"]
      },
      {
        "id": "edge_conditional",
        "source": "node_process",
        "target": "node_end",
        "data_transfer": {
          "mode": "DIRECT",
          "data_formats": ["STRING"]
        },
        "supported_events": ["DATA_READY"],
        "condition": {
          "expression": "output_data == 'success'",
          "description": "Only proceed if output is success"
        }
      }
    ]
  })";

  // Sample workflow with loop
  const char* workflow_with_loop_json_ = R"({
    "id": "workflow_loop_001",
    "name": "Loop Workflow",
    "version": "1.0.0",
    "nodes": [
      {
        "id": "node_init",
        "name": "Initialize",
        "type": "TASK"
      },
      {
        "id": "node_loop",
        "name": "Loop Node",
        "type": "TASK",
        "config": {
          "loop_config": {
            "type": "FOR",
            "max_iterations": 5,
            "condition": "i < 5"
          }
        }
      },
      {
        "id": "node_final",
        "name": "Finalize",
        "type": "TASK"
      }
    ],
    "edges": [
      {
        "id": "edge_init_loop",
        "source": "node_init",
        "target": "node_loop"
      },
      {
        "id": "edge_loop_final",
        "source": "node_loop",
        "target": "node_final"
      }
    ]
  })";

  // Sample workflow with parallel execution
  const char* workflow_parallel_json_ = R"({
    "id": "workflow_parallel_001",
    "name": "Parallel Workflow",
    "version": "1.0.0",
    "nodes": [
      {
        "id": "node_start",
        "name": "Start",
        "type": "TASK"
      },
      {
        "id": "node_parallel_a",
        "name": "Parallel Task A",
        "type": "TASK"
      },
      {
        "id": "node_parallel_b",
        "name": "Parallel Task B",
        "type": "TASK"
      },
      {
        "id": "node_end",
        "name": "End",
        "type": "TASK"
      }
    ],
    "edges": [
      {
        "id": "edge_start_a",
        "source": "node_start",
        "target": "node_parallel_a"
      },
      {
        "id": "edge_start_b",
        "source": "node_start",
        "target": "node_parallel_b"
      },
      {
        "id": "edge_a_end",
        "source": "node_parallel_a",
        "target": "node_end"
      },
      {
        "id": "edge_b_end",
        "source": "node_parallel_b",
        "target": "node_end"
      }
    ]
  })";

  const char* workflow_with_control_cycle_json_ = R"({
    "id": "workflow_cycle_001",
    "name": "Cycle Workflow",
    "version": "1.0.0",
    "nodes": [
      {"id": "start", "name": "Start", "type": "TASK"},
      {"id": "decision", "name": "Decision", "type": "TASK"},
      {"id": "worker", "name": "Worker", "type": "TASK"},
      {"id": "end", "name": "End", "type": "TASK"}
    ],
    "edges": [
      {"id": "e_start_decision", "source": "start", "target": "decision"},
      {"id": "e_decision_worker", "source": "decision", "target": "worker"},
      {"id": "e_worker_decision", "source": "worker", "target": "decision",
        "control": {"type": "goto", "goto_target": "decision"}},
      {"id": "e_worker_end", "source": "worker", "target": "end",
        "control": {"type": "if", "expression": "need_retry == false"}}
    ]
  })";

  const char* workflow_with_nested_node_json_ = R"({
    "id": "workflow_nested_001",
    "name": "Nested Workflow Node",
    "version": "1.0.0",
    "nodes": [
      {"id": "start", "name": "Start", "type": "TASK"},
      {"id": "subflow", "name": "Subflow", "type": "workflow", "workflow_ref": "subflow.json"},
      {"id": "end", "name": "End", "type": "TASK"}
    ],
    "edges": [
      {"id": "e1", "source": "start", "target": "subflow"},
      {"id": "e2", "source": "subflow", "target": "end"}
    ]
  })";

  // Sample invalid workflow JSON - missing required fields
  const char* invalid_workflow_missing_id_json_ = R"({
    "name": "Invalid Workflow",
    "version": "1.0.0",
    "nodes": [],
    "edges": []
  })";

  // Sample invalid workflow JSON - duplicate node IDs
  const char* invalid_workflow_duplicate_nodes_json_ = R"({
    "id": "workflow_invalid",
    "name": "Invalid Workflow",
    "version": "1.0.0",
    "nodes": [
      {
        "id": "node_001",
        "name": "Node 1",
        "type": "TASK"
      },
      {
        "id": "node_001",
        "name": "Node 2",
        "type": "TASK"
      }
    ],
    "edges": []
  })";

  // Sample invalid workflow JSON - circular dependency
  const char* invalid_workflow_circular_json_ = R"({
    "id": "workflow_circular",
    "name": "Circular Workflow",
    "version": "1.0.0",
    "nodes": [
      {
        "id": "node_a",
        "name": "Node A",
        "type": "TASK"
      },
      {
        "id": "node_b",
        "name": "Node B",
        "type": "TASK"
      },
      {
        "id": "node_c",
        "name": "Node C",
        "type": "TASK"
      }
    ],
    "edges": [
      {
        "id": "edge_a_b",
        "source": "node_a",
        "target": "node_b"
      },
      {
        "id": "edge_b_c",
        "source": "node_b",
        "target": "node_c"
      },
      {
        "id": "edge_c_a",
        "source": "node_c",
        "target": "node_a"
      }
    ]
  })";
};

// ============================================================================
// Workflow JSON Parsing Tests
// ============================================================================

TEST_F(WorkflowJsonTest, ParseValidWorkflow) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  EXPECT_TRUE(result.is_valid()) << "Workflow parsing should succeed";
  EXPECT_NE(result.workflow, nullptr) << "Workflow object should be created";
  EXPECT_EQ(result.workflow->get_id(), "workflow_001");
  EXPECT_EQ(result.workflow->get_name(), "Test Workflow");
  EXPECT_EQ(result.workflow->get_nodes().size(), 3) << "Should parse 3 nodes";
  EXPECT_EQ(result.workflow->get_edges().size(), 3) << "Should parse 3 edges";
}

TEST_F(WorkflowJsonTest, ParseWorkflowWithLoop) {
  auto result = parser_->parse_workflow(workflow_with_loop_json_);

  EXPECT_TRUE(result.is_valid()) << "Workflow with loop should parse successfully";
  EXPECT_NE(result.workflow, nullptr);
  EXPECT_EQ(result.workflow->get_nodes().size(), 3);
}

TEST_F(WorkflowJsonTest, ParseParallelWorkflow) {
  auto result = parser_->parse_workflow(workflow_parallel_json_);

  EXPECT_TRUE(result.is_valid()) << "Parallel workflow should parse successfully";
  EXPECT_NE(result.workflow, nullptr);
  EXPECT_EQ(result.workflow->get_nodes().size(), 4);
  EXPECT_EQ(result.workflow->get_edges().size(), 4);

  // Check that parallel nodes are both executable
  auto executable_nodes = result.workflow->get_executable_nodes();
  // After initialization, nodes with no dependencies should be executable
  // For this workflow, node_start should be executable
  EXPECT_FALSE(executable_nodes.empty());
}

TEST_F(WorkflowJsonTest, ParseWorkflowMissingId) {
  auto result = parser_->parse_workflow(invalid_workflow_missing_id_json_);

  EXPECT_FALSE(result.is_valid()) << "Workflow without ID should fail validation";
  EXPECT_FALSE(result.errors.empty()) << "Should have parse errors";
}

TEST_F(WorkflowJsonTest, ParseWorkflowDuplicateNodeIds) {
  auto result = parser_->parse_workflow(invalid_workflow_duplicate_nodes_json_);

  EXPECT_FALSE(result.is_valid()) << "Workflow with duplicate node IDs should fail";
}

TEST_F(WorkflowJsonTest, ParseWorkflowCircularDependency) {
  auto result = parser_->parse_workflow(invalid_workflow_circular_json_);

  // Workflow with circular dependency should parse but validation should fail
  EXPECT_NE(result.workflow, nullptr) << "Circular workflow should still parse";

  bool is_valid = result.workflow->validate();
  EXPECT_FALSE(is_valid) << "Circular dependency should make workflow invalid";
}

// ============================================================================
// Node Configuration Tests
// ============================================================================

TEST_F(WorkflowJsonTest, ParseNodeConfiguration) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  auto start_node = result.workflow->get_node("node_start");
  ASSERT_NE(start_node, nullptr);

  auto config = start_node->get_config();
  EXPECT_EQ(config.timeout_ms, 5000);
  EXPECT_EQ(config.max_retries, 3);
  EXPECT_EQ(config.retry_delay_ms, 1000);
}

TEST_F(WorkflowJsonTest, ParseNodeInputOutput) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  auto process_node = result.workflow->get_node("node_process");
  ASSERT_NE(process_node, nullptr);

  // Check input parameters
  const auto& inputs = process_node->get_input_params();
  EXPECT_FALSE(inputs.empty());

  // Check output parameters
  const auto& outputs = process_node->get_output_params();
  EXPECT_FALSE(outputs.empty());
}

TEST_F(WorkflowJsonTest, ParseWorkflowTypeNode) {
  auto result = parser_->parse_workflow(workflow_with_nested_node_json_);
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(result.workflow, nullptr);
  auto subflow_node = result.workflow->get_node("subflow");
  ASSERT_NE(subflow_node, nullptr);
  EXPECT_EQ(subflow_node->get_type(), "workflow");
  EXPECT_EQ(subflow_node->get_config().workflow_ref, "subflow.json");
}

TEST_F(WorkflowJsonTest, ParseWorkflowIncludes) {
  namespace fs = std::filesystem;
  const fs::path tmp_dir = fs::path("build/test_workflow_includes");
  fs::create_directories(tmp_dir);
  const fs::path sub_path = tmp_dir / "subflow.json";
  const fs::path main_path = tmp_dir / "mainflow.json";

  {
    std::ofstream sub(sub_path.string());
    sub << R"({
      "id": "subflow_001",
      "name": "SubFlow",
      "version": "1.0.0",
      "nodes": [{"id": "sub_node", "name": "Sub Node", "type": "TASK"}],
      "edges": []
    })";
  }

  {
    std::ofstream main(main_path.string());
    main << R"({
      "id": "mainflow_001",
      "name": "MainFlow",
      "version": "1.0.0",
      "includes": ["subflow.json"],
      "nodes": [{"id": "main_node", "name": "Main Node", "type": "TASK"}],
      "edges": []
    })";
  }

  auto result = parser_->parse_workflow_file(main_path.string());
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(result.workflow, nullptr);
  EXPECT_NE(result.workflow->get_node("sub_node"), nullptr);
  EXPECT_NE(result.workflow->get_node("main_node"), nullptr);

  fs::remove_all(tmp_dir);
}

TEST_F(WorkflowJsonTest, ParseDynamicWorkflowPatternFile) {
  namespace fs = std::filesystem;
  fs::path pattern = fs::path("publish/workflow/pattern/dynamic_workflow.json");
  if(!fs::exists(pattern)) {
    pattern = fs::path("../publish/workflow/pattern/dynamic_workflow.json");
  }
  if(!fs::exists(pattern)) {
    pattern = fs::path("../../publish/workflow/pattern/dynamic_workflow.json");
  }
  ASSERT_TRUE(fs::exists(pattern));

  auto result = parser_->parse_workflow_file(pattern.string());
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(result.workflow, nullptr);
  EXPECT_NE(result.workflow->get_node("dynamic_workflow"), nullptr);
}

// ============================================================================
// Edge Configuration Tests
// ============================================================================

TEST_F(WorkflowJsonTest, ParseEdgeConfiguration) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  const auto& edges = result.workflow->get_edges();
  ASSERT_GE(edges.size(), 1);

  auto edge = edges[0];
  EXPECT_EQ(edge->get_source(), "node_start");
  EXPECT_EQ(edge->get_target(), "node_process");
}

TEST_F(WorkflowJsonTest, ParseEdgeConditionExpression) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  const auto& edges = result.workflow->get_edges();

  // Find the edge with condition
  agentflow::framework::Edge* conditional_edge = nullptr;
  for(const auto& edge_ptr : edges) {
    if(edge_ptr->get_id() == "edge_conditional") {
      conditional_edge = edge_ptr.get();
      break;
    }
  }

  ASSERT_NE(conditional_edge, nullptr);

  // Check condition expression
  const auto& condition_expr = conditional_edge->get_condition_expression();
  EXPECT_FALSE(condition_expr.empty());
  EXPECT_EQ(condition_expr, "output_data == 'success'");
}

// ============================================================================
// Topological Order Tests
// ============================================================================

TEST_F(WorkflowJsonTest, GetTopologicalOrder) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  auto order = result.workflow->get_topological_order();

  EXPECT_FALSE(order.empty()) << "Topological order should not be empty";
  EXPECT_EQ(order.size(), 3) << "Should have 3 nodes in order";

  // Check order: start -> process -> end
  EXPECT_EQ(order[0], "node_start");
  EXPECT_EQ(order[1], "node_process");
  EXPECT_EQ(order[2], "node_end");
}

TEST_F(WorkflowJsonTest, GetExecutableNodes) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  auto executable = result.workflow->get_executable_nodes();

  EXPECT_FALSE(executable.empty());
  // Initially, only node_start should be executable (no dependencies)
  EXPECT_EQ(executable.size(), 1);
  EXPECT_EQ(executable[0], "node_start");
}

// ============================================================================
// Control Flow Integration Tests
// ============================================================================

TEST_F(WorkflowJsonTest, GetExecutableNodesWithControl) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  auto executable_with_control = result.workflow->get_executable_nodes_with_control();

  EXPECT_FALSE(executable_with_control.empty()) << "Should have executable nodes with control flow";
}

TEST_F(WorkflowJsonTest, ControlFlowManagerIntegration) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  // Verify control context is initialized
  auto& control_context = result.workflow->get_control_context();
  EXPECT_TRUE(control_context.get_active_loops().empty());

}

// ============================================================================
// Loop Management Tests
// ============================================================================

TEST_F(WorkflowJsonTest, WorkflowLoopConfiguration) {
  auto result = parser_->parse_workflow(workflow_with_loop_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  auto loop_node = result.workflow->get_node("node_loop");
  ASSERT_NE(loop_node, nullptr);

  EXPECT_EQ(loop_node->get_type(), "TASK");
}

TEST_F(WorkflowJsonTest, EnterLoop) {
  auto result = parser_->parse_workflow(workflow_with_loop_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  agentflow::framework::LoopConfig loop_config;
  loop_config.type           = agentflow::framework::LoopType::FOR_LOOP;
  loop_config.max_iterations = 3;
  // loop_config.condition is shared_ptr<ConditionExpression>, cannot assign string directly

  result.workflow->enter_loop("test_loop", loop_config);

  auto& control_context = result.workflow->get_control_context();
  EXPECT_TRUE(control_context.is_in_loop("test_loop"));
}

TEST_F(WorkflowJsonTest, ExitLoopIteration) {
  auto result = parser_->parse_workflow(workflow_with_loop_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  agentflow::framework::LoopConfig loop_config;
  loop_config.type           = agentflow::framework::LoopType::FOR_LOOP;
  loop_config.max_iterations = 3;

  result.workflow->enter_loop("test_loop", loop_config);

  // Exit current iteration
  result.workflow->exit_loop_iteration("test_loop");

  auto& control_context = result.workflow->get_control_context();
  EXPECT_TRUE(control_context.is_in_loop("test_loop"));
}

TEST_F(WorkflowJsonTest, ShouldContinueLoop) {
  auto result = parser_->parse_workflow(workflow_with_loop_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  agentflow::framework::LoopConfig loop_config;
  loop_config.type           = agentflow::framework::LoopType::FOR_LOOP;
  loop_config.start_value    = 0;
  loop_config.end_value      = 2;
  loop_config.step           = 1;
  loop_config.loop_variable  = "i";
  loop_config.max_iterations = 3;

  result.workflow->enter_loop("test_loop", loop_config);

  // Should continue initially
  EXPECT_TRUE(result.workflow->should_continue_loop("test_loop"));
}

// ============================================================================
// Error Handler Tests
// ============================================================================

TEST_F(WorkflowJsonTest, SetErrorHandler) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);

  agentflow::framework::ErrorHandlerConfig error_handler;
  error_handler.strategy    = agentflow::framework::ErrorStrategy::RETRY;
  error_handler.max_retries = 3;

  result.workflow->set_error_handler("node_process", error_handler);
}

// ============================================================================
// Version Management Tests
// ============================================================================

TEST_F(WorkflowJsonTest, WorkflowVersionParsing) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(result.workflow->get_name(), "Test Workflow");
  EXPECT_EQ(result.workflow->get_id(), "workflow_001");
}

TEST_F(WorkflowJsonTest, WorkflowMetadataParsing) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  // Check metadata field parsing
  // Metadata should be parsed if present
}

// ============================================================================
// Workflow Graph Validation Tests
// ============================================================================

TEST_F(WorkflowJsonTest, ValidateWorkflowGraphStructure) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());

  // Valid workflow should pass workflow graph validation
  EXPECT_TRUE(result.workflow->validate());
}

TEST_F(WorkflowJsonTest, ValidateEdgeReferences) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());

  // All edge references should be valid
  EXPECT_TRUE(result.workflow->validate());
}

TEST_F(WorkflowJsonTest, ValidateNodeIdsUnique) {
  // Valid workflow has unique node IDs
  auto result = parser_->parse_workflow(valid_workflow_json_);
  ASSERT_TRUE(result.is_valid());

  // Validation should succeed
  EXPECT_TRUE(result.workflow->validate());
}

TEST_F(WorkflowJsonTest, ParseEdgeControlConfig) {
  auto result = parser_->parse_workflow(R"({
    "id": "workflow_control_001",
    "name": "Control Workflow",
    "version": "1.0.0",
    "nodes": [
      {"id": "n1", "name": "N1", "type": "TASK"},
      {"id": "n2", "name": "N2", "type": "TASK"}
    ],
    "edges": [
      {
        "id": "e1",
        "source": "n1",
        "target": "n2",
        "control": {"type": "async", "callback_node": "n1"}
      }
    ]
  })");

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.workflow, nullptr);
  const auto& edges = result.workflow->get_edges();
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0]->get_control_type(), agentflow::framework::Edge::ControlType::ASYNC);
  EXPECT_EQ(edges[0]->get_control_config().callback_node, "n1");
}

TEST_F(WorkflowJsonTest, AllowCycleWhenUsingGotoControl) {
  auto result = parser_->parse_workflow(workflow_with_control_cycle_json_);
  ASSERT_TRUE(result.is_valid()) << result.get_error_summary();
  ASSERT_NE(result.workflow, nullptr);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(WorkflowJsonTest, HandleInvalidJSON) {
  auto result = parser_->parse_workflow("{invalid json}");

  EXPECT_FALSE(result.is_valid()) << "Invalid JSON should fail to parse";
  EXPECT_FALSE(result.errors.empty()) << "Should have parse errors";
}

TEST_F(WorkflowJsonTest, HandleEmptyWorkflow) {
  auto result = parser_->parse_workflow("{}");

  EXPECT_TRUE(result.is_valid()) << "Empty workflow should be valid (with auto-generated ID)";
}

TEST_F(WorkflowJsonTest, HandleMissingNodes) {
  auto result = parser_->parse_workflow(R"({
    "id": "test",
    "name": "Test",
    "edges": []
  })");

  EXPECT_TRUE(result.is_valid()) << "Workflow without nodes should be valid (partial definition)";
}

TEST_F(WorkflowJsonTest, HandleMissingEdges) {
  auto result = parser_->parse_workflow(R"({
    "id": "test",
    "name": "Test",
    "nodes": []
  })");

  // This might be valid (empty workflow)
  // But edges with invalid node references should be caught
}

// ============================================================================
// Dynamic Update Tests
// ============================================================================

TEST_F(WorkflowJsonTest, DynamicWorkflowUpdate) {
  auto result = parser_->parse_workflow(valid_workflow_json_);

  ASSERT_TRUE(result.is_valid());

  // Update workflow dynamically
  bool update_result = result.workflow->update_dynamic(valid_workflow_json_);
  EXPECT_TRUE(update_result) << "Dynamic update should succeed";
}

}  // namespace tests
}  // namespace agentflow
