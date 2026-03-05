#include <cassert>

#include "workflow/edge.h"
#include "workflow/node.h"
#include "workflow/workflow.h"

using namespace agentflow;

void test_workflow() {
  // Test MMWorkflow creation
  framework::MMWorkflow::Config config;
  config.id      = "test_workflow";
  config.name    = "TestWorkflow";
  config.version = "1.0";

  auto workflow = std::make_shared<framework::MMWorkflow>(config);

  // Test getters
  assert(workflow->get_id() == "test_workflow");
  assert(workflow->get_name() == "TestWorkflow");
  assert(workflow->get_state() == WorkflowState::CREATED);

  // Test adding nodes
  framework::MMNode::Config node_config1;
  node_config1.id   = "node1";
  node_config1.name = "Node1";
  node_config1.type = "test";

  auto node1 = std::make_shared<framework::MMNode>(node_config1);
  workflow->add_node(node1);

  framework::MMNode::Config node_config2;
  node_config2.id   = "node2";
  node_config2.name = "Node2";
  node_config2.type = "test";

  auto node2 = std::make_shared<framework::MMNode>(node_config2);
  workflow->add_node(node2);

  assert(workflow->get_nodes().size() == 2);
  assert(workflow->get_node("node1") != nullptr);
  assert(workflow->get_node("node2") != nullptr);

  // Test adding edges (node1 -> node2, no cycle)
  framework::MMEdge::Config edge_config;
  edge_config.id          = "edge1";
  edge_config.source_node = "node1";
  edge_config.target_node = "node2";

  auto edge1 = std::make_shared<framework::MMEdge>(edge_config);
  workflow->add_edge(edge1);

  assert(workflow->get_edges().size() == 1);

  // Test validation
  assert(workflow->validate());
  assert(workflow->get_state() == WorkflowState::LOADED);

  // Test removal
  workflow->remove_node("node1");
  assert(workflow->get_nodes().size() == 1);
}
