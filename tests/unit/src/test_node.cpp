#include <cassert>

#include "workflow/node.h"

using namespace agentflow;

void test_node() {
  // Test MMNode creation
  framework::MMNode::Config config;
  config.id          = "test_node";
  config.name        = "TestNode";
  config.type        = "test";
  config.max_retries = 3;
  config.timeout_ms  = 1000;
  config.cpp_function = [](const DataMap& inputs, DataMap& outputs) -> bool {
    // Simple test function that just returns success
    (void)inputs;
    outputs["result"] = true;
    return true;
  };

  auto node = std::make_shared<framework::MMNode>(config);

  // Test getters
  assert(node->get_id() == "test_node");
  assert(node->get_name() == "TestNode");
  assert(node->get_state() == NodeState::WAITING);

  // Test initialization
  assert(node->initialize());
  assert(node->get_state() == NodeState::READY);

  // Test execution
  assert(node->execute());
  assert(node->get_state() == NodeState::SUCCESS);

  // Test cleanup
  assert(node->cleanup());
  assert(node->get_state() == NodeState::WAITING);
}
