// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// trace_context.h — Thread-local TraceContext and RAII ScopedSpan.
//
// Each worker thread carries a TraceContext that maintains a stack of active
// span IDs.  ScopedSpan is the primary API — construct one on the stack and
// it automatically opens/closes the span via RAII.
//
// Usage example:
//   {
//     ScopedSpan sp(SpanKind::NODE, "my-node",
//                   {{"agent_id","a1"},{"workflow_id","w1"}});
//     sp.add_event("cache_miss");
//     // ... do work ...
//     sp.set_status(SpanStatus::OK);
//   }  // automatically records end_us and flushes to TraceManager

#ifndef AGENTFLOW_RUNTIME_TRACE_CONTEXT_H
#define AGENTFLOW_RUNTIME_TRACE_CONTEXT_H

#include <string>
#include <map>

#include "runtime/trace/trace_types.h"

namespace agentflow {
namespace runtime {
namespace trace {

// ============================================================================
// TraceContext — thread-local state
// ============================================================================
class TraceContext {
public:
  static TraceContext& current();  // returns thread_local instance

  // Active trace / span stack
  const std::string& trace_id()     const { return trace_id_; }
  const std::string& span_id()      const { return current_span_id_; }
  const std::string& parent_span_id() const { return parent_span_id_; }

  // Called by ScopedSpan constructor
  void push(const std::string& trace_id,
            const std::string& span_id);

  // Called by ScopedSpan destructor
  void pop(const std::string& span_id);

  // Inject a trace from an external source (e.g., propagate from request)
  void inject(const std::string& trace_id, const std::string& parent_span_id);

  void clear();

private:
  std::string trace_id_;
  std::string current_span_id_;
  std::string parent_span_id_;

  // Simple stack: [trace_id, span_id] pairs
  struct Frame {
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
  };
  std::vector<Frame> stack_;
};

// ============================================================================
// ScopedSpan — RAII span wrapper (primary user-facing API)
// ============================================================================
class ScopedSpan {
public:
  // Construct and immediately start the span.
  // If no trace is active in this thread (TraceContext::trace_id() empty)
  // AND no matching TraceTask exists, this is a no-op (is_active() == false).
  ScopedSpan(SpanKind kind,
             const std::string& name,
             const std::map<std::string, std::string>& tags = {});

  // Non-copyable, non-movable
  ScopedSpan(const ScopedSpan&) = delete;
  ScopedSpan& operator=(const ScopedSpan&) = delete;

  ~ScopedSpan();

  bool        is_active()  const { return active_; }
  const std::string& span_id()   const { return span_.span_id; }
  const std::string& trace_id()  const { return span_.trace_id; }

  void set_status(SpanStatus s, const std::string& err_msg = "");
  void set_tag(const std::string& key, const std::string& value);
  void add_event(const std::string& name,
                 const std::map<std::string, std::string>& attrs = {});

private:
  bool      active_ = false;
  TraceSpan span_;
};

// ============================================================================
// Helper: generate a short random id
// ============================================================================
std::string generate_id();

}  // namespace trace
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_TRACE_CONTEXT_H
