#include <cassert>

#include "agentflow/framework/edge.h"
#include "agentflow/framework/node.h"
#include "agentflow/framework/workflow.h"

using namespace agentflow;

void test_workflow() {
  // Test Workflow creation
  framework::Workflow::Config config;
  config.id      = "test_workflow";
  config.name    = "TestWorkflow";
  config.version = "1.0";

  auto workflow = std::make_shared<framework::Workflow>(config);

  // Test getters
  assert(workflow->get_id() == "test_workflow");
  assert(workflow->get_name() == "TestWorkflow");
  assert(workflow->get_state() == WorkflowState::CREATED);

  // Test adding nodes
  framework::Node::Config node_config;
  node_config.id   = "node1";
  node_config.name = "Node1";
  node_config.type = "test";

  auto node1 = std::make_shared<framework::Node>(node_config);
  workflow->add_node(node1);

  assert(workflow->get_nodes().size() == 1);
  assert(workflow->get_node("node1") != nullptr);

  // Test adding edges
  framework::Edge::Config edge_config;
  edge_config.id          = "edge1";
  edge_config.source_node = "node1";
  edge_config.target_node = "node1";

  auto edge1 = std::make_shared<framework::Edge>(edge_config);
  workflow->add_edge(edge1);

  assert(workflow->get_edges().size() == 1);

  // Test validation
  assert(workflow->validate());
  assert(workflow->get_state() == WorkflowState::LOADED);

  // Test removal
  workflow->remove_node("node1");
  assert(workflow->get_nodes().size() == 0);
}
