#include "utils/logger.h"
#include "agent/agent.h"
#include "workflow/node.h"
#include "workflow/workflow.h"
#include "runtime/scheduler/agent_runtime.h"

using namespace agentflow;

int main() {
  auto& logger = Logger::instance();
  logger.set_level(Logger::Level::INFO);

  logger.info("AgentFlow Basic Example Started");

  // Create an agent
  framework::Agent::Config agent_config;
  agent_config.id          = "agent_001";
  agent_config.name        = "BasicAgent";
  agent_config.description = "A basic example agent";
  agent_config.policy      = framework::Agent::LifecyclePolicy::SINGLETON;

  auto agent = std::make_shared<framework::Agent>(agent_config);
  logger.info("Agent created");

  // Create a workflow
  framework::Workflow::Config workflow_config;
  workflow_config.id      = "workflow_001";
  workflow_config.name    = "BasicWorkflow";
  workflow_config.version = "1.0";

  auto workflow = std::make_shared<framework::Workflow>(workflow_config);
  logger.info("Workflow created");

  // Create nodes
  framework::Node::Config node1_config;
  node1_config.id          = "node_001";
  node1_config.name        = "DataProcessingNode";
  node1_config.type        = "processor";
  node1_config.max_retries = 3;
  node1_config.timeout_ms  = 5000;

  auto node1 = std::make_shared<framework::Node>(node1_config);
  workflow->add_node(node1);
  logger.info("Node 1 added to workflow");

  framework::Node::Config node2_config;
  node2_config.id   = "node_002";
  node2_config.name = "OutputNode";
  node2_config.type = "output";

  auto node2 = std::make_shared<framework::Node>(node2_config);
  workflow->add_node(node2);
  logger.info("Node 2 added to workflow");

  // Create edge between nodes
  framework::Edge::Config edge_config;
  edge_config.id            = "edge_001";
  edge_config.source_node   = "node_001";
  edge_config.target_node   = "node_002";
  edge_config.transfer_mode = framework::Edge::DataTransferMode::SHARED_MEMORY;
  edge_config.source_outputs.push_back("output");
  edge_config.target_inputs.push_back("input");

  auto edge = std::make_shared<framework::Edge>(edge_config);
  workflow->add_edge(edge);
  logger.info("Edge added to workflow");

  // Add workflow to agent
  agent->add_workflow(workflow);
  logger.info("Workflow added to agent");

  // Initialize and start agent
  if(!agent->initialize()) {
    logger.error("Failed to initialize agent");
    return 1;
  }
  logger.info("Agent initialized");

  if(!agent->start()) {
    logger.error("Failed to start agent");
    return 1;
  }
  logger.info("Agent started");

  // Test data bus
  agent->publish_data("test_key", "test_value");
  DataVariant retrieved_value;
  if(agent->get_data("test_key", retrieved_value)) {
    logger.info("Data bus test: successfully retrieved data");
  }

  // Test workflow info
  auto retrieved_workflow = agent->get_workflow("workflow_001");
  if(retrieved_workflow) {
    logger.info("Successfully retrieved workflow from agent");
    logger.info_f("Workflow nodes count: %d",
                  static_cast<int>(retrieved_workflow->get_nodes().size()));
  }

  // Stop agent
  if(!agent->stop()) {
    logger.error("Failed to stop agent");
    return 1;
  }
  logger.info("Agent stopped");

  logger.info("AgentFlow Basic Example Completed Successfully");
  return 0;
}
