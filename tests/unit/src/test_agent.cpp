#include <cassert>

#include "agent/agent.h"
#include "workflow/workflow.h"

using namespace agentflow;

void test_agent() {
  // Test Agent creation
  framework::Agent::Config config;
  config.id     = "test_agent";
  config.name   = "TestAgent";
  config.policy = framework::Agent::LifecyclePolicy::SINGLETON;

  auto agent = std::make_shared<framework::Agent>(config);

  // Test getters
  assert(agent->get_id() == "test_agent");
  assert(agent->get_name() == "TestAgent");
  assert(agent->get_state() == AgentState::CREATED);

  // Test data bus operations
  DataVariant value = std::string("test_value");
  assert(agent->publish_data("key1", value));

  DataVariant retrieved;
  assert(agent->get_data("key1", retrieved));
  assert(std::get<std::string>(retrieved) == "test_value");

  // Test event queue operations
  agent->post_event("event1");
  std::string event = agent->get_event();
  assert(event == "event1");

  // Test workflow management
  framework::Workflow::Config wf_config;
  wf_config.id   = "workflow1";
  wf_config.name = "Workflow1";

  auto workflow = std::make_shared<framework::Workflow>(wf_config);
  agent->add_workflow(workflow);

  assert(agent->get_workflows().size() == 1);
  assert(agent->get_workflow("workflow1") != nullptr);

  // Test lifecycle
  assert(agent->initialize());
  assert(agent->get_state() == AgentState::INITIALIZED);

  assert(agent->start());
  assert(agent->get_state() == AgentState::RUNNING);

  assert(agent->stop());
  assert(agent->get_state() == AgentState::STOPPED);
}
