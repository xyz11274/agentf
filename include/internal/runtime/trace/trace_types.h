// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// trace_types.h — Core data types for the AgentFlow full-chain tracing system.
//
// Hierarchy:
//   TraceTask    — user-created monitoring task (filter + lifecycle)
//   TraceSession — one end-to-end request trace (many spans)
//   TraceSpan    — one execution unit (AGENT / WORKFLOW / NODE / EDGE)
//   SpanEvent    — timestamped event within a span (start/end/error/custom)

#ifndef AGENTFLOW_RUNTIME_TRACE_TYPES_H
#define AGENTFLOW_RUNTIME_TRACE_TYPES_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentflow {
namespace runtime {
namespace trace {

// ============================================================================
// Timestamps (microseconds since epoch)
// ============================================================================
using TimestampUs = int64_t;

inline TimestampUs now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

// ============================================================================
// SpanKind — what kind of object does this span represent
// ============================================================================
enum class SpanKind {
  REQUEST  = 0,  // top-level user request (injected at agent entry)
  AGENT    = 1,  // MMAgent instance
  WORKFLOW = 2,  // MMWorkflow execution
  NODE     = 3,  // MMNode execution
  EDGE     = 4,  // edge traversal (future use)
  CUSTOM   = 9,
};

inline std::string span_kind_name(SpanKind k) {
  switch (k) {
    case SpanKind::REQUEST:  return "REQUEST";
    case SpanKind::AGENT:    return "AGENT";
    case SpanKind::WORKFLOW: return "WORKFLOW";
    case SpanKind::NODE:     return "NODE";
    case SpanKind::EDGE:     return "EDGE";
    default:                 return "CUSTOM";
  }
}

// ============================================================================
// SpanStatus
// ============================================================================
enum class SpanStatus { RUNNING = 0, OK = 1, ERROR = 2, CANCELLED = 3 };

inline std::string span_status_name(SpanStatus s) {
  switch (s) {
    case SpanStatus::RUNNING:   return "RUNNING";
    case SpanStatus::OK:        return "OK";
    case SpanStatus::ERROR:     return "ERROR";
    case SpanStatus::CANCELLED: return "CANCELLED";
    default:                    return "UNKNOWN";
  }
}

// ============================================================================
// SpanEvent — a timestamped point within a span
// ============================================================================
struct SpanEvent {
  TimestampUs                        timestamp_us = 0;
  std::string                        name;
  std::map<std::string, std::string> attributes;
};

// ============================================================================
// TraceSpan — one execution unit in a trace tree
// ============================================================================
struct TraceSpan {
  std::string  trace_id;
  std::string  span_id;
  std::string  parent_span_id;  // empty → root
  SpanKind     kind       = SpanKind::CUSTOM;
  std::string  name;            // human-readable label
  uint64_t     thread_id  = 0;

  // Timing
  TimestampUs  start_us   = 0;
  TimestampUs  end_us     = 0;
  int64_t      duration_us() const { return end_us > 0 ? end_us - start_us : 0; }

  SpanStatus   status     = SpanStatus::RUNNING;
  std::string  error_msg;

  // Tags (searchable key-value pairs)
  std::map<std::string, std::string> tags;

  // Events within this span
  std::vector<SpanEvent> events;

  // Resource usage snapshot (lightweight, sampled at span end)
  double cpu_percent  = 0.0;  // placeholder — populated if available
  int64_t rss_kb      = 0;    // RSS memory in KB
};

// ============================================================================
// TraceSession — all spans for a single end-to-end request
// ============================================================================
struct TraceSession {
  std::string trace_id;
  std::string task_id;      // which TraceTask captured this
  std::string request_id;   // matches MMAgentMsgQue::Message::message_id
  std::string user_id;
  std::string client_id;
  std::string session_id;   // agent session id

  TimestampUs start_us = 0;
  TimestampUs end_us   = 0;

  std::vector<TraceSpan> spans;
};

// ============================================================================
// TraceTaskFilter — conditions for which requests to capture
// An empty field means "match all".
// ============================================================================
struct TraceTaskFilter {
  std::string session_id;   // match by agent session id
  std::string user_id;
  std::string client_id;
  std::string message_id;   // match by specific message id
  std::string agent_id;     // match by agent id
  std::string workflow_id;  // match by workflow id
  int         max_sessions = 0;  // 0 = unlimited
};

// ============================================================================
// TraceTask — user-created monitoring task
// ============================================================================
enum class TraceTaskState { CREATED = 0, RUNNING = 1, STOPPED = 2, DELETED = 3 };

inline std::string trace_task_state_name(TraceTaskState s) {
  switch (s) {
    case TraceTaskState::CREATED: return "CREATED";
    case TraceTaskState::RUNNING: return "RUNNING";
    case TraceTaskState::STOPPED: return "STOPPED";
    case TraceTaskState::DELETED: return "DELETED";
    default:                      return "UNKNOWN";
  }
}

struct TraceTask {
  std::string     task_id;
  std::string     name;
  TraceTaskFilter filter;
  TraceTaskState  state        = TraceTaskState::CREATED;
  TimestampUs     created_at   = 0;
  TimestampUs     started_at   = 0;
  TimestampUs     stopped_at   = 0;
  int             session_count = 0;  // how many sessions captured so far
  std::string     output_dir;         // where NDJSON files are written
};

}  // namespace trace
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_TRACE_TYPES_H
