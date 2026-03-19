// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test tests for MMWorkflow component

#include <gtest/gtest.h>

#include "workflow/workflow.h"
#include "workflow/edge.h"
#include "workflow/node.h"

using namespace agentflow;  // Bring agentflow namespace into scope

class WorkflowTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(WorkflowTest, CreateWorkflow) {
  // TODO: Implement workflow creation test
  EXPECT_TRUE(true);
}

TEST_F(WorkflowTest, TopologicalOrder) {
  // TODO: Implement topological ordering test
  EXPECT_TRUE(true);
}

TEST_F(WorkflowTest, ExecuteWorkflow) {
  // TODO: Implement workflow execution test
  EXPECT_TRUE(true);
}

// =============================================================================
// Scenario 2 Test: Multi-Incoming Edge Data Mapping
// =============================================================================
// Verifies that when multiple nodes output the same key and all point to
// the same downstream node, the data is properly isolated via key prefixing.

TEST_F(WorkflowTest, EdgeTransferDataMapping) {
  // Test explicit source_outputs/target_inputs mapping
  framework::MMEdge::Config edge_config;
  edge_config.id = "edge_a_to_c";
  edge_config.source_node = "node_a";
  edge_config.target_node = "node_c";
  edge_config.source_outputs = {"result"};
  edge_config.target_inputs = {"result_from_a"};

  auto edge = std::make_shared<framework::MMEdge>(edge_config);

  DataMap source_data = {{"result", std::string("A")}, {"other", std::string("ignored")}};
  DataMap target_data;

  bool result = edge->transfer_data(source_data, target_data);

  EXPECT_TRUE(result);
  EXPECT_EQ(target_data.size(), 1u);
  EXPECT_NE(target_data.find("result_from_a"), target_data.end());
  if(target_data.find("result_from_a") != target_data.end()) {
    const auto* val = std::get_if<std::string>(&target_data["result_from_a"]);
    EXPECT_NE(val, nullptr);
    if(val) {
      EXPECT_EQ(*val, "A");
    }
  }
}

TEST_F(WorkflowTest, EdgeTransferZeroConfigPassthrough) {
  // Test zero-config: source_outputs empty means pass through without mapping
  framework::MMEdge::Config edge_config;
  edge_config.id = "edge_simple";
  edge_config.source_node = "node_source";
  edge_config.target_node = "node_target";
  // source_outputs and target_inputs are empty - zero-config mode

  auto edge = std::make_shared<framework::MMEdge>(edge_config);

  DataMap source_data = {{"key1", std::string("value1")}, {"key2", std::string("value2")}};
  DataMap target_data;

  // When source_outputs is empty, transfer_data does nothing
  // The runtime handles zero-config passthrough separately
  bool result = edge->transfer_data(source_data, target_data);

  // With empty source_outputs, transfer_data returns true but transfers nothing
  EXPECT_TRUE(result);
  EXPECT_EQ(target_data.size(), 0u);
}

TEST_F(WorkflowTest, MultiIncomingEdgeCount) {
  // Test that we can count incoming edges to a node
  framework::MMWorkflow::Config workflow_config;
  workflow_config.id = "test_workflow";
  auto workflow = std::make_shared<framework::MMWorkflow>(workflow_config);

  // Create nodes
  framework::MMNode::Config node_a_config;
  node_a_config.id = "node_a";
  node_a_config.name = "Node A";

  framework::MMNode::Config node_b_config;
  node_b_config.id = "node_b";
  node_b_config.name = "Node B";

  framework::MMNode::Config node_c_config;
  node_c_config.id = "node_c";
  node_c_config.name = "Node C (merge)";

  auto node_a = std::make_shared<framework::MMNode>(node_a_config);
  auto node_b = std::make_shared<framework::MMNode>(node_b_config);
  auto node_c = std::make_shared<framework::MMNode>(node_c_config);

  workflow->add_node(node_a);
  workflow->add_node(node_b);
  workflow->add_node(node_c);

  // Create edges: node_a -> node_c, node_b -> node_c
  framework::MMEdge::Config edge_a_c_config;
  edge_a_c_config.id = "edge_a_c";
  edge_a_c_config.source_node = "node_a";
  edge_a_c_config.target_node = "node_c";

  framework::MMEdge::Config edge_b_c_config;
  edge_b_c_config.id = "edge_b_c";
  edge_b_c_config.source_node = "node_b";
  edge_b_c_config.target_node = "node_c";

  auto edge_a_c = std::make_shared<framework::MMEdge>(edge_a_c_config);
  auto edge_b_c = std::make_shared<framework::MMEdge>(edge_b_c_config);

  workflow->add_edge(edge_a_c);
  workflow->add_edge(edge_b_c);

  // Count incoming edges to node_c
  int incoming_count = 0;
  for(const auto& edge : workflow->get_edges()) {
    if(edge && edge->get_target() == "node_c") {
      ++incoming_count;
    }
  }

  EXPECT_EQ(incoming_count, 2);
}

TEST_F(WorkflowTest, OutgoingEdgeDetection) {
  // Test that we can detect if a node has downstream edges
  framework::MMWorkflow::Config workflow_config;
  workflow_config.id = "test_workflow_outgoing";
  auto workflow = std::make_shared<framework::MMWorkflow>(workflow_config);

  framework::MMNode::Config node_a_config;
  node_a_config.id = "node_a";
  node_a_config.name = "Node A";

  framework::MMNode::Config node_b_config;
  node_b_config.id = "node_b";
  node_b_config.name = "Node B";

  auto node_a = std::make_shared<framework::MMNode>(node_a_config);
  auto node_b = std::make_shared<framework::MMNode>(node_b_config);

  workflow->add_node(node_a);
  workflow->add_node(node_b);

  framework::MMEdge::Config edge_config;
  edge_config.id = "edge_a_b";
  edge_config.source_node = "node_a";
  edge_config.target_node = "node_b";

  auto edge = std::make_shared<framework::MMEdge>(edge_config);
  workflow->add_edge(edge);

  // Check node_a has downstream
  bool has_downstream = false;
  for(const auto& e : workflow->get_edges()) {
    if(e && e->get_source() == node_a->get_id()) {
      has_downstream = true;
      break;
    }
  }
  EXPECT_TRUE(has_downstream);

  // Check node_b has no downstream
  has_downstream = false;
  for(const auto& e : workflow->get_edges()) {
    if(e && e->get_source() == node_b->get_id()) {
      has_downstream = true;
      break;
    }
  }
  EXPECT_FALSE(has_downstream);
}
