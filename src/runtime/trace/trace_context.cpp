// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "runtime/trace/trace_context.h"

#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>

#include "runtime/trace/trace_manager.h"

namespace agentflow {
namespace runtime {
namespace trace {

// ============================================================================
// ID generation
// ============================================================================
std::string generate_id() {
  static thread_local std::mt19937_64 rng(
      std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
      static_cast<uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::uniform_int_distribution<uint64_t> dist;
  uint64_t hi = dist(rng);
  uint64_t lo = dist(rng);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0')
      << std::setw(16) << hi
      << std::setw(16) << lo;
  return oss.str();
}

// ============================================================================
// TraceContext — thread_local
// ============================================================================
thread_local static TraceContext tl_context;

TraceContext& TraceContext::current() {
  return tl_context;
}

void TraceContext::push(const std::string& trace_id, const std::string& span_id) {
  Frame f;
  f.trace_id       = trace_id;
  f.span_id        = span_id;
  f.parent_span_id = current_span_id_;
  stack_.push_back(f);
  trace_id_       = trace_id;
  parent_span_id_ = current_span_id_;
  current_span_id_ = span_id;
}

void TraceContext::pop(const std::string& span_id) {
  if (stack_.empty()) return;
  if (stack_.back().span_id != span_id) {
    // Mismatched pop — clear to avoid corruption
    stack_.clear();
    trace_id_.clear();
    current_span_id_.clear();
    parent_span_id_.clear();
    return;
  }
  stack_.pop_back();
  if (stack_.empty()) {
    trace_id_.clear();
    current_span_id_.clear();
    parent_span_id_.clear();
  } else {
    const Frame& top = stack_.back();
    trace_id_        = top.trace_id;
    current_span_id_ = top.span_id;
    parent_span_id_  = top.parent_span_id;
  }
}

void TraceContext::inject(const std::string& trace_id, const std::string& parent_span_id) {
  trace_id_        = trace_id;
  parent_span_id_  = parent_span_id;
  current_span_id_ = parent_span_id;
  stack_.clear();
  if (!trace_id.empty()) {
    Frame f;
    f.trace_id       = trace_id;
    f.span_id        = parent_span_id;
    f.parent_span_id = "";
    stack_.push_back(f);
  }
}

void TraceContext::clear() {
  stack_.clear();
  trace_id_.clear();
  current_span_id_.clear();
  parent_span_id_.clear();
}

// ============================================================================
// ScopedSpan
// ============================================================================
ScopedSpan::ScopedSpan(SpanKind kind,
                       const std::string& name,
                       const std::map<std::string, std::string>& tags) {
  auto& ctx = TraceContext::current();

  // Use existing trace context, or check if TraceManager has a matching task
  std::string tid = ctx.trace_id();
  if (tid.empty()) {
    // No active trace on this thread — nothing to record
    active_ = false;
    return;
  }

  active_ = true;
  span_.trace_id       = tid;
  span_.span_id        = generate_id();
  span_.parent_span_id = ctx.span_id();
  span_.kind           = kind;
  span_.name           = name;
  span_.tags           = tags;
  span_.start_us       = now_us();
  span_.status         = SpanStatus::RUNNING;

  // Capture thread id
  span_.thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id())
                    & 0xFFFFFFFFull;

  ctx.push(tid, span_.span_id);
}

ScopedSpan::~ScopedSpan() {
  if (!active_) return;

  span_.end_us = now_us();
  if (span_.status == SpanStatus::RUNNING) {
    span_.status = SpanStatus::OK;
  }

  TraceContext::current().pop(span_.span_id);
  TraceManager::instance().record_span(span_);
}

void ScopedSpan::set_status(SpanStatus s, const std::string& err_msg) {
  if (!active_) return;
  span_.status    = s;
  span_.error_msg = err_msg;
}

void ScopedSpan::set_tag(const std::string& key, const std::string& value) {
  if (!active_) return;
  span_.tags[key] = value;
}

void ScopedSpan::add_event(const std::string& name,
                           const std::map<std::string, std::string>& attrs) {
  if (!active_) return;
  SpanEvent ev;
  ev.timestamp_us = now_us();
  ev.name         = name;
  ev.attributes   = attrs;
  span_.events.push_back(std::move(ev));
}

}  // namespace trace
}  // namespace runtime
}  // namespace agentflow
