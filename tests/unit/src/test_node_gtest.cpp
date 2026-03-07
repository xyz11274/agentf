// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include "workflow/node.h"

using namespace agentflow;
using namespace agentflow::framework;

class NodeTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(NodeTest, CreateNodeWithConfig) {
  MMNode::Config config;
  config.id = "test_node";
  config.name = "test_node";
  config.type = "process";
  config.priority = 10;
  config.timeout_ms = 5000;
  config.max_retries = 3;

  MMNode node(config);

  EXPECT_EQ(node.get_id(), "test_node");
  EXPECT_EQ(node.get_name(), "test_node");
  EXPECT_EQ(node.get_state(), NodeState::WAITING);
}

TEST_F(NodeTest, NodeStateTransitions) {
  MMNode::Config config;
  config.id = "node_1";
  config.name = "node_1";
  config.type = "Process";

  MMNode node(config);

  EXPECT_EQ(node.get_state(), NodeState::WAITING);
  node.set_state(NodeState::READY);
  EXPECT_EQ(node.get_state(), NodeState::READY);
  node.set_state(NodeState::EXECUTING);
  EXPECT_EQ(node.get_state(), NodeState::EXECUTING);
  node.set_state(NodeState::SUCCESS);
  EXPECT_EQ(node.get_state(), NodeState::SUCCESS);
}

TEST_F(NodeTest, NodeDataHandling) {
  MMNode::Config config;
  config.id = "data_node";
  config.name = "data_node";
  config.type = "DataProcessor";

  MMNode node(config);

  DataMap input;
  input["key1"] = std::string("value1");
  input["key2"] = std::string("value2");
  node.set_inputs(input);

  const DataMap& retrieved_input = node.get_inputs();
  EXPECT_EQ(retrieved_input.size(), 2);
  auto compare_variant = [](const DataVariant& v, const std::string& expected) {
    return std::visit(
        [&expected](auto&& arg) -> bool {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) {
            return arg == expected;
          }
          return false;
        },
        v);
  };
  EXPECT_TRUE(compare_variant(retrieved_input.at("key1"), "value1"));
  EXPECT_TRUE(compare_variant(retrieved_input.at("key2"), "value2"));

  DataMap output;
  output["result"] = std::string("success");
  node.set_outputs(output);
  const DataMap& retrieved_output = node.get_outputs();
  EXPECT_TRUE(compare_variant(retrieved_output.at("result"), "success"));
}

TEST_F(NodeTest, NodeLifecycle) {
  MMNode::Config config;
  config.id = "lifecycle_node";
  config.name = "lifecycle_node";
  config.type = "LifecycleTest";
  config.cpp_function = [](const DataMap&, DataMap& outputs) -> bool {
    outputs["result"] = std::string("success");
    return true;
  };

  MMNode node(config);

  EXPECT_EQ(node.get_state(), NodeState::WAITING);
  EXPECT_TRUE(node.initialize());
  EXPECT_TRUE(node.execute());
  EXPECT_TRUE(node.cleanup());
}

TEST_F(NodeTest, MultipleNodes) {
  MMNode::Config config1;
  config1.id = "node_1";
  config1.name = "node_1";
  config1.type = "Source";

  MMNode::Config config2;
  config2.id = "node_2";
  config2.name = "node_2";
  config2.type = "Filter";

  MMNode::Config config3;
  config3.id = "node_3";
  config3.name = "node_3";
  config3.type = "Sink";

  MMNode node1(config1);
  MMNode node2(config2);
  MMNode node3(config3);

  EXPECT_EQ(node1.get_id(), "node_1");
  EXPECT_EQ(node2.get_id(), "node_2");
  EXPECT_EQ(node3.get_id(), "node_3");

  node1.set_state(NodeState::EXECUTING);
  EXPECT_EQ(node1.get_state(), NodeState::EXECUTING);
  EXPECT_EQ(node2.get_state(), NodeState::WAITING);
  EXPECT_EQ(node3.get_state(), NodeState::WAITING);
}
