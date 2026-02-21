#include <iostream>
#include <memory>

#include "agent/agent.h"
#include "agent/session.h"
#include "agent/task.h"
#include "agent/msgque.h"
#include "agent/context.h"
#include "agent/event.h"
#include "agent/package.h"

using namespace agentflow::framework;

/**
 * Simple example demonstrating the new agent features with components
 */
int main() {
  std::cout << "=== AgentFlow Agent Components Example ===" << std::endl;

  // Create an agent configuration
  Agent::Config agent_config;
  agent_config.id = "example_agent_001";
  agent_config.name = "Example Agent";
  agent_config.version = "1.0.0";
  agent_config.description = "Simple agent demonstrating new components";
  agent_config.policy = Agent::LifecyclePolicy::SINGLETON;
  agent_config.max_workflows = 5;

  // Create the agent
  auto agent = std::make_shared<Agent>(agent_config);
  std::cout << "\n1. Created Agent: " << agent->get_name() << " (ID: " << agent->get_id() << ")"
            << std::endl;

  // Access and use the session component
  auto session = agent->get_session();
  if(session) {
    std::cout << "\n2. Session Component:" << std::endl;
    session->activate();
    std::cout << "   - Session activated" << std::endl;
    session->set_metadata("user", "demo_user");
    std::cout << "   - Metadata set: user=demo_user" << std::endl;
  }

  // Access and use the context component
  auto context = agent->get_context();
  if(context) {
    std::cout << "\n3. Context Component:" << std::endl;
    context->set_property("environment", "development");
    context->set_env_variable("DEBUG", "true");
    std::cout << "   - Context property set: environment=development" << std::endl;
    std::cout << "   - Environment variable set: DEBUG=true" << std::endl;
  }

  // Access and use the messages component
  auto messages = agent->get_messages();
  if(messages) {
    std::cout << "\n4. Messages Component:" << std::endl;
    AgentMsgQue::Message msg;
    msg.message_id = "msg_001";
    msg.type = AgentMsgQue::MessageType::TASK_REQUEST;
    msg.sender_id = agent->get_id();
    msg.receiver_id = "target_agent";
    msg.content = "Hello from agent example";
    messages->enqueue_message(msg);
    std::cout << "   - Message enqueued: " << msg.message_id << std::endl;
    std::cout << "   - Queue size: " << messages->get_queue_size() << std::endl;
  }

  // Access and use the event component
  auto event_handler = agent->get_event_handler();
  if(event_handler) {
    std::cout << "\n5. Event Component:" << std::endl;
    
    // Register an event listener
    auto listener_id = event_handler->register_listener(
        AgentEvent::EventType::TASK_STARTED,
        [](const AgentEvent::EventData& event) {
          std::cout << "   - Event received: " << event.event_id << std::endl;
        });
    
    // Emit an event
    AgentEvent::EventData event_data;
    event_data.event_id = "event_001";
    event_data.type = AgentEvent::EventType::TASK_STARTED;
    event_data.source_id = "example_agent_001";
    event_data.description = "Task started";
    event_handler->emit_event(event_data);
    
    std::cout << "   - Event emitted and handled" << std::endl;
  }

  // Access and use the task component
  auto task = agent->get_current_task();
  if(task) {
    std::cout << "\n6. Task Component:" << std::endl;
    task->set_parameter("input_data", "sample_input");
    task->start();
    std::cout << "   - Task started" << std::endl;
    task->set_progress(0.5);
    std::cout << "   - Task progress: 50%" << std::endl;
    task->complete("task completed successfully");
    std::cout << "   - Task completed with result: " << task->get_result() << std::endl;
  }

  // Access and use the package component
  auto package = agent->get_package();
  if(package) {
    std::cout << "\n7. Package Component:" << std::endl;
    package->create();
    package->add_resource("/path/to/resource1.bin");
    package->add_resource("/path/to/resource2.bin");
    std::cout << "   - Package created with " << package->get_resources().size()
              << " resources" << std::endl;
    
    AgentPackage::Dependency dep;
    dep.name = "dependency_lib";
    dep.version = "1.0.0";
    package->add_dependency(dep);
    std::cout << "   - Dependency added: " << dep.name << "@" << dep.version << std::endl;
  }

  // Initialize the agent
  std::cout << "\n8. Agent Lifecycle:" << std::endl;
  if(agent->initialize()) {
    std::cout << "   - Agent initialized successfully" << std::endl;
  }

  if(agent->start()) {
    std::cout << "   - Agent started" << std::endl;
  }

  if(agent->pause()) {
    std::cout << "   - Agent paused" << std::endl;
  }

  if(agent->resume()) {
    std::cout << "   - Agent resumed" << std::endl;
  }

  if(agent->stop()) {
    std::cout << "   - Agent stopped" << std::endl;
  }

  std::cout << "\n=== Example Complete ===" << std::endl;

  return 0;
}
