// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// flow_trace.h — Flow trace logger for Agent scheduling visualization.
//
// This is a separate system from the existing trace/ system.
// The existing trace system is for performance analysis and call chain tracing.
// This flow_trace system is specifically for scheduling flow visualization.
//
// Output format: JSON lines for easy parsing and visualization.

#ifndef AGENTFLOW_RUNTIME_FLOW_TRACE_H
#define AGENTFLOW_RUNTIME_FLOW_TRACE_H

#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace agentflow {
namespace runtime {

// ============================================================================
// FlowEventType — the type of flow event
// ============================================================================
enum class FlowEventType {
  TASK_CREATED,
  TASK_DISPATCHED,
  WORKFLOW_SCHEDULED,
  WORKFLOW_STARTED,
  WORKFLOW_COMPLETED,
  NODE_STARTED,
  NODE_COMPLETED,
  DATA_EXCHANGE,
  SKILL_LAUNCHED,
  SKILL_COMPLETED,
  RESPONSE_SENT,
  DIALOGUE_CREATED,
  DIALOGUE_COMPLETED,
  EXTERNAL_CALL
};

inline std::string flow_event_type_name(FlowEventType t) {
  switch (t) {
    case FlowEventType::TASK_CREATED:
      return "task-created";
    case FlowEventType::TASK_DISPATCHED:
      return "task-dispatched";
    case FlowEventType::WORKFLOW_SCHEDULED:
      return "workflow-scheduled";
    case FlowEventType::WORKFLOW_STARTED:
      return "workflow-started";
    case FlowEventType::WORKFLOW_COMPLETED:
      return "workflow-completed";
    case FlowEventType::NODE_STARTED:
      return "node-started";
    case FlowEventType::NODE_COMPLETED:
      return "node-completed";
    case FlowEventType::DATA_EXCHANGE:
      return "data-exchange";
    case FlowEventType::SKILL_LAUNCHED:
      return "skill-launched";
    case FlowEventType::SKILL_COMPLETED:
      return "skill-completed";
    case FlowEventType::RESPONSE_SENT:
      return "response-sent";
    case FlowEventType::DIALOGUE_CREATED:
      return "dialogue-created";
    case FlowEventType::DIALOGUE_COMPLETED:
      return "dialogue-completed";
    case FlowEventType::EXTERNAL_CALL:
      return "external-call";
    default:
      return "unknown";
  }
}

// ============================================================================
// FlowTraceEvent — a single flow trace event
// ============================================================================
struct FlowTraceEvent {
  // Timestamp (formatted string for display)
  std::string time;  // "2026-03-13 18:23:55.660"

  // Type category (for filtering)
  std::string type;  // "task-exec" | "node-exec" | "workflow-exec" | "skill-exec"

  // Context identifiers
  std::string session_id;
  std::string dialogue_id;
  std::string task_id;    // optional, for task-level tracking
  std::string trace_id;   // optional, for correlation with trace system

  // Flow information
  std::string from_entity;  // "builtin-dispatcher"
  std::string to_entity;    // "skill-selection"
  std::string event_type;   // "task-created"

  // Optional data (JSON strings)
  std::string inputs;   // JSON string or empty
  std::string outputs;  // JSON string or empty

  // Timing (for duration calculation)
  int64_t timestamp_us = 0;  // raw microsecond timestamp
  int64_t duration_us  = 0;  // duration if this is a completion event

  // Additional attributes
  std::map<std::string, std::string> attributes;
};

// Convert event to JSON line (for output)
std::string to_json_line(const FlowTraceEvent& event);

// Format timestamp to human-readable string
std::string format_timestamp(int64_t timestamp_us);

// Get current timestamp in microseconds
int64_t now_us();

// ============================================================================
// FlowTraceWriter — singleton writer for flow trace events
// ============================================================================
class FlowTraceWriter {
 public:
  static FlowTraceWriter& instance();

  // Configuration
  void set_output_file(const std::string& path);
  void set_stdout_enabled(bool enabled);
  void set_enabled(bool enabled);

  // Check if tracing is enabled
  bool is_enabled() const { return enabled_; }

  // Write an event
  void write_event(const FlowTraceEvent& event);

  // Flush output
  void flush();

 private:
  FlowTraceWriter() = default;
  ~FlowTraceWriter();
  FlowTraceWriter(const FlowTraceWriter&) = delete;
  FlowTraceWriter& operator=(const FlowTraceWriter&) = delete;

  mutable std::mutex mutex_;
  std::ofstream output_file_;
  bool enabled_        = true;
  bool stdout_enabled_ = true;
  bool file_enabled_   = false;
};

// ============================================================================
// Helper functions to create events
// ============================================================================

// Create a basic flow event with timestamp
FlowTraceEvent make_flow_event(const std::string& type,
                                const std::string& session_id,
                                const std::string& dialogue_id,
                                const std::string& from_entity,
                                const std::string& to_entity,
                                FlowEventType event_type);

// Create a task-level event
FlowTraceEvent make_task_event(const std::string& session_id,
                                const std::string& dialogue_id,
                                const std::string& from_entity,
                                const std::string& to_entity,
                                FlowEventType event_type);

// Create a workflow-level event
FlowTraceEvent make_workflow_event(const std::string& session_id,
                                    const std::string& dialogue_id,
                                    const std::string& from_entity,
                                    const std::string& to_entity,
                                    FlowEventType event_type);

// Create a node-level event
FlowTraceEvent make_node_event(const std::string& session_id,
                                const std::string& dialogue_id,
                                const std::string& from_entity,
                                const std::string& to_entity,
                                FlowEventType event_type);

// ============================================================================
// Convenience macros for flow tracing
// ============================================================================

#define FLOW_TRACE_EVENT(type, session, dialogue, from, to, event_type, attrs)       \
  do {                                                                                \
    if (agentflow::runtime::FlowTraceWriter::instance().is_enabled()) {               \
      auto _e = agentflow::runtime::make_flow_event(                                  \
          type, session, dialogue, from, to, event_type);                             \
      _e.attributes = attrs;                                                          \
      agentflow::runtime::FlowTraceWriter::instance().write_event(_e);                \
    }                                                                                 \
  } while (0)

#define FLOW_TRACE_TASK(session, dialogue, from, to, event_type)                      \
  FLOW_TRACE_EVENT("task-exec", session, dialogue, from, to, event_type, {})

#define FLOW_TRACE_WORKFLOW(session, dialogue, from, to, event_type)                  \
  FLOW_TRACE_EVENT("workflow-exec", session, dialogue, from, to, event_type, {})

#define FLOW_TRACE_NODE(session, dialogue, from, to, event_type)                      \
  FLOW_TRACE_EVENT("node-exec", session, dialogue, from, to, event_type, {})

#define FLOW_TRACE_SKILL(session, dialogue, from, to, event_type)                     \
  FLOW_TRACE_EVENT("skill-exec", session, dialogue, from, to, event_type, {})

// Macros with inputs/outputs
#define FLOW_TRACE_TASK_WITH_DATA(session, dialogue, from, to, event_type, in, out)   \
  do {                                                                                \
    if (agentflow::runtime::FlowTraceWriter::instance().is_enabled()) {               \
      auto _e = agentflow::runtime::make_task_event(                                  \
          session, dialogue, from, to, event_type);                                   \
      _e.inputs  = in;                                                                \
      _e.outputs = out;                                                               \
      agentflow::runtime::FlowTraceWriter::instance().write_event(_e);                \
    }                                                                                 \
  } while (0)

#define FLOW_TRACE_NODE_WITH_DATA(session, dialogue, from, to, event_type, in, out,   \
                                   duration)                                          \
  do {                                                                                \
    if (agentflow::runtime::FlowTraceWriter::instance().is_enabled()) {               \
      auto _e = agentflow::runtime::make_node_event(                                  \
          session, dialogue, from, to, event_type);                                   \
      _e.inputs      = in;                                                            \
      _e.outputs     = out;                                                           \
      _e.duration_us = duration;                                                      \
      agentflow::runtime::FlowTraceWriter::instance().write_event(_e);                \
    }                                                                                 \
  } while (0)

#define FLOW_TRACE_WORKFLOW_WITH_DATA(session, dialogue, from, to, event_type, in,    \
                                       out, duration)                                 \
  do {                                                                                \
    if (agentflow::runtime::FlowTraceWriter::instance().is_enabled()) {               \
      auto _e = agentflow::runtime::make_workflow_event(                              \
          session, dialogue, from, to, event_type);                                   \
      _e.inputs      = in;                                                            \
      _e.outputs     = out;                                                           \
      _e.duration_us = duration;                                                      \
      agentflow::runtime::FlowTraceWriter::instance().write_event(_e);                \
    }                                                                                 \
  } while (0)

/// Trace external callback invocation
/// @param session_id Session identifier
/// @param dialogue_id Dialogue identifier
/// @param from Source entity (typically agent-id)
/// @param to Target entity (e.g., "ctx-callback")
/// @param event Event name ("enter-callback" or "exit-callback")
/// @param in_data Input to callback (user_id|agent_id|user_input|previous_context)
/// @param out_data Output from callback (generated context or error message)
#define FLOW_TRACE_EXTERNAL_CALL(session, dialogue, from, to, event, in_data, out_data) \
  do {                                                                                \
    if (agentflow::runtime::FlowTraceWriter::instance().is_enabled()) {               \
      auto _e = agentflow::runtime::make_flow_event(                                  \
          "external-call", session, dialogue, from, to,                               \
          agentflow::runtime::FlowEventType::EXTERNAL_CALL);                          \
      _e.inputs = in_data;                                                            \
      _e.outputs = out_data;                                                          \
      _e.attributes["event"] = event;                                                 \
      agentflow::runtime::FlowTraceWriter::instance().write_event(_e);                \
    }                                                                                 \
  } while (0)

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_FLOW_TRACE_H