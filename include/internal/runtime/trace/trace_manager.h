// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// trace_manager.h — TraceManager singleton.
//
// Responsibilities:
//  - TraceTask lifecycle: create / list / start / stop / delete
//  - TraceSession recording: begin_session / record_span / end_session
//  - Persistence: flush completed sessions to NDJSON files
//  - Sampling gate: is_tracing(session_id/user_id/...) — only record when
//    a matching running task exists

#ifndef AGENTFLOW_RUNTIME_TRACE_MANAGER_H
#define AGENTFLOW_RUNTIME_TRACE_MANAGER_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/trace/trace_types.h"

namespace agentflow {
namespace runtime {
namespace trace {

// ============================================================================
// SamplingContext — passed to is_tracing() to check filter matching
// ============================================================================
struct SamplingContext {
  std::string session_id;
  std::string user_id;
  std::string client_id;
  std::string message_id;
  std::string agent_id;
  std::string workflow_id;
};

// ============================================================================
// TraceManager — singleton
// ============================================================================
class TraceManager {
public:
  static TraceManager& instance();

  // ---- Configuration -------------------------------------------------------
  void set_output_dir(const std::string& dir);
  const std::string& output_dir() const { return output_dir_; }

  // ---- TraceTask CRUD -------------------------------------------------------
  std::string  create_task(const std::string& name, const TraceTaskFilter& filter);
  bool         start_task(const std::string& task_id);
  bool         stop_task(const std::string& task_id);
  bool         delete_task(const std::string& task_id);

  TraceTask*   get_task(const std::string& task_id);
  std::vector<TraceTask> list_tasks() const;

  // ---- Sampling gate -------------------------------------------------------
  // Returns matching task_id if any running task matches ctx, else "".
  std::string  find_task(const SamplingContext& ctx) const;
  bool         is_tracing(const SamplingContext& ctx) const;

  // ---- Session lifecycle ---------------------------------------------------
  // begin_session: call at request entry; returns trace_id.
  std::string  begin_session(const std::string& task_id,
                              const std::string& request_id,
                              const std::string& user_id,
                              const std::string& client_id,
                              const std::string& session_id);

  // record_span: called by ScopedSpan destructor.
  void         record_span(const TraceSpan& span);

  // end_session: called after request completes; flushes to file.
  void         end_session(const std::string& trace_id);

  TraceSession* get_session(const std::string& trace_id);  // null if not found

  // ---- Data access ---------------------------------------------------------
  std::vector<TraceSession> list_sessions(const std::string& task_id) const;

  // Get all spans for a task across all completed sessions (for export)
  std::vector<TraceSpan>    get_spans(const std::string& task_id) const;

  // ---- Persistence ---------------------------------------------------------
  // Flush a completed session to NDJSON (one span per line).
  bool         flush_session(const std::string& trace_id);
  // Flush all completed in-memory sessions for a task.
  void         flush_task(const std::string& task_id);

private:
  TraceManager();
  ~TraceManager() = default;
  TraceManager(const TraceManager&) = delete;
  TraceManager& operator=(const TraceManager&) = delete;

  bool filter_matches(const TraceTaskFilter& f, const SamplingContext& ctx) const;
  std::string next_id();

  // Feed a completed span into the global MetricsCollector
  void feed_metrics(const TraceSpan& span);

  // Write metrics.json snapshot to the given directory
  void flush_metrics_snapshot(const std::string& dir);

  std::string output_dir_ = "trace_output";

  mutable std::mutex              tasks_mutex_;
  std::map<std::string, TraceTask> tasks_;

  mutable std::mutex                   sessions_mutex_;
  std::map<std::string, TraceSession>  active_sessions_;    // trace_id → session
  std::map<std::string, std::vector<TraceSession>> completed_;  // task_id → sessions

  std::atomic<uint64_t> id_counter_{0};

  void flush_session_internal(const TraceSession& sess);
};

}  // namespace trace
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_TRACE_MANAGER_H
