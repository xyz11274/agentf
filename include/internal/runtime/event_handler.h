#ifndef AGENTFLOW_RUNTIME_EVENT_HANDLER_H
#define AGENTFLOW_RUNTIME_EVENT_HANDLER_H

#include <memory>
#include <string>
#include <vector>

namespace agentflow {
namespace runtime {

/**
 * EventType enumeration for runtime lifecycle events.
 * Events are grouped by entity type with numeric gaps for future extensions.
 */
enum class EventType {
  // Agent lifecycle events (0-9)
  AGENT_REGISTERED = 0,
  AGENT_UNREGISTERED = 1,

  // Session lifecycle events (10-19)
  SESSION_CREATED = 10,
  SESSION_DESTROYED = 11,

  // Dialogue lifecycle events (20-29)
  DIALOGUE_CREATED = 20,
  DIALOGUE_DESTROYED = 21,

  // Task lifecycle events (30-39)
  TASK_CREATED = 30,
  TASK_DESTROYED = 31,
};

/**
 * Node information in workflow topology.
 * Represents a single node's static definition from the workflow DSL.
 */
struct NodeInfo {
  std::string id;
  std::string name;
  std::string type;
  int         priority = 0;

  // Input/Output parameter definitions
  std::vector<std::string> input_params;
  std::vector<std::string> output_params;
};

/**
 * Edge information in workflow topology.
 * Represents a connection between two nodes with optional condition.
 */
struct EdgeInfo {
  std::string id;
  std::string source_node_id;
  std::string target_node_id;
  std::string condition;  // Optional condition expression
};

/**
 * Workflow information with expanded topology.
 * Contains the full workflow definition including all nodes and edges.
 */
struct WorkflowInfo {
  std::string id;
  std::string name;
  std::string version;
  std::string description;

  // Expanded topology (not just file names)
  std::vector<NodeInfo> nodes;
  std::vector<EdgeInfo> edges;
};

/**
 * Agent event data.
 * Emitted when an agent is registered or unregistered.
 * Contains the complete workflow topology for all workflows in the agent.
 */
struct AgentEvent {
  EventType   event_type;  // AGENT_REGISTERED, AGENT_UNREGISTERED
  std::string agent_id;
  std::string name;
  std::string version;
  std::string description;

  // Expanded workflow topology
  std::vector<WorkflowInfo> workflows;
};

/**
 * Session event data.
 * Emitted when a session is created or destroyed.
 */
struct SessionEvent {
  EventType   event_type;  // SESSION_CREATED, SESSION_DESTROYED
  std::string agent_id;
  std::string user_id;
  std::string client_id;
  std::string session_id;
};

/**
 * Dialogue event data.
 * Emitted when a dialogue is created or destroyed.
 */
struct DialogueEvent {
  EventType   event_type;  // DIALOGUE_CREATED, DIALOGUE_DESTROYED
  std::string agent_id;
  std::string user_id;
  std::string client_id;
  std::string session_id;
  std::string dialogue_id;
  std::string user_input;
};

/**
 * Task event data.
 * Emitted when a task is created or destroyed.
 * Contains skill information for skill-type tasks.
 */
struct TaskEvent {
  EventType   event_type;  // TASK_CREATED, TASK_DESTROYED
  std::string agent_id;
  std::string user_id;
  std::string client_id;
  std::string session_id;
  std::string dialogue_id;
  std::string task_id;
  std::string workflow_id;
  std::string task_type;       // dispatcher, coordinator, skill
  std::string skill_name;      // Valid for skill type only
};

/**
 * Interface for event handlers.
 * Implement this interface to receive runtime lifecycle events.
 *
 * Usage:
 * 1. Create a class implementing IEventHandler
 * 2. Register with MMAgentRuntime::register_event_handler()
 * 3. Events will be delivered asynchronously via thread pool
 *
 * Note: Handlers are called from a thread pool, ensure thread safety.
 */
class IEventHandler {
public:
  virtual ~IEventHandler() = default;

  /**
   * Handle agent lifecycle events.
   * Called when AGENT_REGISTERED or AGENT_UNREGISTERED occurs.
   */
  virtual void handle_agent_event(const AgentEvent& event) = 0;

  /**
   * Handle session lifecycle events.
   * Called when SESSION_CREATED or SESSION_DESTROYED occurs.
   */
  virtual void handle_session_event(const SessionEvent& event) = 0;

  /**
   * Handle dialogue lifecycle events.
   * Called when DIALOGUE_CREATED or DIALOGUE_DESTROYED occurs.
   */
  virtual void handle_dialogue_event(const DialogueEvent& event) = 0;

  /**
   * Handle task lifecycle events.
   * Called when TASK_CREATED or TASK_DESTROYED occurs.
   */
  virtual void handle_task_event(const TaskEvent& event) = 0;
};

/**
 * Helper class for building event structures.
 * Provides convenient factory methods for creating events.
 */
class EventBuilder {
public:
  static AgentEvent build_agent_registered_event(
      const std::string& agent_id,
      const std::string& name,
      const std::string& version,
      const std::string& description,
      const std::vector<WorkflowInfo>& workflows) {
    AgentEvent event;
    event.event_type  = EventType::AGENT_REGISTERED;
    event.agent_id    = agent_id;
    event.name        = name;
    event.version     = version;
    event.description = description;
    event.workflows   = workflows;
    return event;
  }

  static AgentEvent build_agent_unregistered_event(const std::string& agent_id) {
    AgentEvent event;
    event.event_type = EventType::AGENT_UNREGISTERED;
    event.agent_id   = agent_id;
    return event;
  }

  static SessionEvent build_session_created_event(
      const std::string& agent_id,
      const std::string& user_id,
      const std::string& client_id,
      const std::string& session_id) {
    SessionEvent event;
    event.event_type = EventType::SESSION_CREATED;
    event.agent_id   = agent_id;
    event.user_id    = user_id;
    event.client_id  = client_id;
    event.session_id = session_id;
    return event;
  }

  static SessionEvent build_session_destroyed_event(
      const std::string& agent_id,
      const std::string& session_id) {
    SessionEvent event;
    event.event_type = EventType::SESSION_DESTROYED;
    event.agent_id   = agent_id;
    event.session_id = session_id;
    return event;
  }

  static DialogueEvent build_dialogue_created_event(
      const std::string& agent_id,
      const std::string& user_id,
      const std::string& client_id,
      const std::string& session_id,
      const std::string& dialogue_id,
      const std::string& user_input) {
    DialogueEvent event;
    event.event_type  = EventType::DIALOGUE_CREATED;
    event.agent_id    = agent_id;
    event.user_id     = user_id;
    event.client_id   = client_id;
    event.session_id  = session_id;
    event.dialogue_id = dialogue_id;
    event.user_input  = user_input;
    return event;
  }

  static DialogueEvent build_dialogue_destroyed_event(
      const std::string& agent_id,
      const std::string& session_id,
      const std::string& dialogue_id) {
    DialogueEvent event;
    event.event_type  = EventType::DIALOGUE_DESTROYED;
    event.agent_id    = agent_id;
    event.session_id  = session_id;
    event.dialogue_id = dialogue_id;
    return event;
  }

  static TaskEvent build_task_created_event(
      const std::string& agent_id,
      const std::string& user_id,
      const std::string& client_id,
      const std::string& session_id,
      const std::string& dialogue_id,
      const std::string& task_id,
      const std::string& workflow_id,
      const std::string& task_type,
      const std::string& skill_name      = "") {
    TaskEvent event;
    event.event_type       = EventType::TASK_CREATED;
    event.agent_id         = agent_id;
    event.user_id          = user_id;
    event.client_id        = client_id;
    event.session_id       = session_id;
    event.dialogue_id      = dialogue_id;
    event.task_id          = task_id;
    event.workflow_id      = workflow_id;
    event.task_type        = task_type;
    event.skill_name       = skill_name;
    return event;
  }

  static TaskEvent build_task_destroyed_event(
      const std::string& agent_id,
      const std::string& session_id,
      const std::string& dialogue_id,
      const std::string& task_id) {
    TaskEvent event;
    event.event_type  = EventType::TASK_DESTROYED;
    event.agent_id    = agent_id;
    event.session_id  = session_id;
    event.dialogue_id = dialogue_id;
    event.task_id     = task_id;
    return event;
  }
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_EVENT_HANDLER_H