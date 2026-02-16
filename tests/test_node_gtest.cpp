// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test unit tests for Node component

#include <gtest/gtest.h>
#include "agentflow/framework/node.h"

namespace agentflow {

class NodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Setup test fixtures
  }

  void TearDown() override {
    // Cleanup
  }
};

// Test Node creation and basic properties
TEST_F(NodeTest, CreateNodeWithConfig) {
  NodeConfig config("test_node", "process");
  config.priority = 10;
  config.timeout_ms = 5000;
  config.max_retries = 3;

  Node node(config);

  EXPECT_EQ(node.get_id(), "test_node");
  EXPECT_EQ(node.get_name(), "test_node");
  EXPECT_EQ(node.get_state(), NodeState::WAITING);
}

// Test Node state transitions
TEST_F(NodeTest, NodeStateTransitions) {
  Node node("node_1", "Process");

  EXPECT_EQ(node.get_state(), NodeState::WAITING);

  // Transition to READY
  node.set_state(NodeState::READY);
  EXPECT_EQ(node.get_state(), NodeState::READY);

  // Transition to EXECUTING
  node.set_state(NodeState::EXECUTING);
  EXPECT_EQ(node.get_state(), NodeState::EXECUTING);

  // Transition to SUCCESS
  node.set_state(NodeState::SUCCESS);
  EXPECT_EQ(node.get_state(), NodeState::SUCCESS);
}

// Test Node with input/output data
TEST_F(NodeTest, NodeDataHandling) {
  Node node("data_node", "DataProcessor");

  DataMap input;
  input["key1"] = "value1";
  input["key2"] = "value2";

  node.set_input_data(input);

  DataMap retrieved_input = node.get_input_data();
  EXPECT_EQ(retrieved_input.size(), 2);
  EXPECT_EQ(retrieved_input["key1"], "value1");
  EXPECT_EQ(retrieved_input["key2"], "value2");

  // Set output data
  DataMap output;
  output["result"] = "success";
  node.set_output_data(output);

  DataMap retrieved_output = node.get_output_data();
  EXPECT_EQ(retrieved_output["result"], "success");
}

// Test Node config with error strategy
TEST_F(NodeTest, NodeErrorStrategy) {
  NodeConfig config("error_node", "ErrorHandler");
  config.error_strategy = ErrorStrategy::RETRY;
  config.max_retries = 5;

  Node node(config);

  EXPECT_EQ(node.get_name(), "error_node");
  EXPECT_EQ(node.get_state(), NodeState::WAITING);
}

// Test Node lifecycle
TEST_F(NodeTest, NodeLifecycle) {
  Node node("lifecycle_node", "LifecycleTest");

  // Initial state
  EXPECT_EQ(node.get_state(), NodeState::WAITING);

  // Initialize
  Status status = node.initialize(Status::OK);
  EXPECT_EQ(status, Status::OK);

  // Execute
  dataMap input_data;
  status = node.execute(input_data);
  EXPECT_EQ(status, Status::OK);
  EXPECT_EQ(node.get_state(), NodeState::EXECUTING);

  // Cleanup
  status = node.cleanup();
  EXPECT_EQ(status, Status::OK);
}

// Test multiple nodes
TEST_F(NodeTest, MultipleNodes) {
  Node node1("node_1", "Source");
  Node node2("node_2", "Filter");
  Node node3("node_3", "Sink");

  EXPECT_EQ(node1.get_id(), "node_1");
  EXPECT_EQ(node2.get_id(), "node_2");
  EXPECT_EQ(node3.get_id(), "node_3");

  // Each should be independent
  node1.set_state(NodeState::EXECUTING);
  EXPECT_EQ(node1.get_state(), NodeState::EXECUTING);
  EXPECT_EQ(node2.get_state(), NodeState::WAITING);
  EXPECT_EQ(node3.get_state(), NodeState::WAITING);
}

// Test Node priority
TEST_F(NodeTest, NodePriority) {
  NodeConfig config1("high_priority", "Task");
  config1.priority = 100;

  NodeConfig config2("low_priority", "Task");
  config2.priority = 1;

  Node node_high(config1);
  Node node_low(config2);

  EXPECT_GT(node_high.get_priority(), node_low.get_priority());
}

}  // namespace agentflow
