// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "runtime/trace/trace_manager.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "runtime/trace/trace_context.h"
#include "runtime/trace/trace_exporter.h"
#include "runtime/metrics.h"

namespace agentflow {
namespace runtime {
namespace trace {

// ============================================================================
// Singleton
// ============================================================================
TraceManager& TraceManager::instance() {
  static TraceManager inst;
  return inst;
}

TraceManager::TraceManager() = default;

// ============================================================================
// Configuration
// ============================================================================
void TraceManager::set_output_dir(const std::string& dir) {
  output_dir_ = dir;
}

// ============================================================================
// ID generation
// ============================================================================
std::string TraceManager::next_id() {
  uint64_t n = id_counter_.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream oss;
  oss << std::hex << n;
  return oss.str();
}

// ============================================================================
// TraceTask CRUD
// ============================================================================
std::string TraceManager::create_task(const std::string& name,
                                      const TraceTaskFilter& filter) {
  TraceTask task;
  task.task_id    = generate_id().substr(0, 16);
  task.name       = name;
  task.filter     = filter;
  task.state      = TraceTaskState::CREATED;
  task.created_at = now_us();
  task.output_dir = output_dir_;

  std::lock_guard<std::mutex> lock(tasks_mutex_);
  tasks_[task.task_id] = task;
  return task.task_id;
}

bool TraceManager::start_task(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(tasks_mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return false;
  if (it->second.state == TraceTaskState::DELETED) return false;
  it->second.state      = TraceTaskState::RUNNING;
  it->second.started_at = now_us();
  return true;
}

bool TraceManager::stop_task(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(tasks_mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return false;
  it->second.state      = TraceTaskState::STOPPED;
  it->second.stopped_at = now_us();
  return true;
}

bool TraceManager::delete_task(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(tasks_mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return false;
  it->second.state = TraceTaskState::DELETED;
  tasks_.erase(it);
  return true;
}

TraceTask* TraceManager::get_task(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(tasks_mutex_);
  auto it = tasks_.find(task_id);
  return it != tasks_.end() ? &it->second : nullptr;
}

std::vector<TraceTask> TraceManager::list_tasks() const {
  std::lock_guard<std::mutex> lock(tasks_mutex_);
  std::vector<TraceTask> result;
  result.reserve(tasks_.size());
  for (const auto& kv : tasks_) result.push_back(kv.second);
  return result;
}

// ============================================================================
// Filter matching
// ============================================================================
bool TraceManager::filter_matches(const TraceTaskFilter& f,
                                  const SamplingContext& ctx) const {
  if (!f.session_id.empty()  && f.session_id  != ctx.session_id)  return false;
  if (!f.user_id.empty()     && f.user_id     != ctx.user_id)     return false;
  if (!f.client_id.empty()   && f.client_id   != ctx.client_id)   return false;
  if (!f.message_id.empty()  && f.message_id  != ctx.message_id)  return false;
  if (!f.agent_id.empty()    && f.agent_id    != ctx.agent_id)    return false;
  if (!f.workflow_id.empty() && f.workflow_id != ctx.workflow_id) return false;
  return true;
}

std::string TraceManager::find_task(const SamplingContext& ctx) const {
  std::lock_guard<std::mutex> lock(tasks_mutex_);
  for (const auto& kv : tasks_) {
    if (kv.second.state == TraceTaskState::RUNNING &&
        filter_matches(kv.second.filter, ctx)) {
      return kv.first;
    }
  }
  return "";
}

bool TraceManager::is_tracing(const SamplingContext& ctx) const {
  return !find_task(ctx).empty();
}

// ============================================================================
// Session lifecycle
// ============================================================================
std::string TraceManager::begin_session(const std::string& task_id,
                                         const std::string& request_id,
                                         const std::string& user_id,
                                         const std::string& client_id,
                                         const std::string& session_id) {
  std::string trace_id = generate_id().substr(0, 24);

  TraceSession sess;
  sess.trace_id   = trace_id;
  sess.task_id    = task_id;
  sess.request_id = request_id;
  sess.user_id    = user_id;
  sess.client_id  = client_id;
  sess.session_id = session_id;
  sess.start_us   = now_us();

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    active_sessions_[trace_id] = std::move(sess);
  }

  // Update task session count
  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) it->second.session_count++;
  }

  // Inject trace into current thread's context
  TraceContext::current().inject(trace_id, "");
  return trace_id;
}

void TraceManager::record_span(const TraceSpan& span) {
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = active_sessions_.find(span.trace_id);
    if (it != active_sessions_.end()) {
      it->second.spans.push_back(span);
    }
  }
  // Feed aggregate metrics (no lock needed; MetricsCollector is thread-safe)
  feed_metrics(span);
}

void TraceManager::end_session(const std::string& trace_id) {
  TraceSession sess;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = active_sessions_.find(trace_id);
    if (it == active_sessions_.end()) return;
    sess = std::move(it->second);
    active_sessions_.erase(it);
  }
  sess.end_us = now_us();

  // Update active_sessions gauge
  {
    auto mc = MetricsCollectorFactory::get_default();
    auto g = std::dynamic_pointer_cast<GaugeMetric>(mc->get_metric("agentflow.active_sessions"));
    if (!g) {
      g = mc->create_gauge("agentflow.active_sessions", "Number of in-flight trace sessions");
    }
    std::lock_guard<std::mutex> lk(sessions_mutex_);
    g->set(static_cast<double>(active_sessions_.size()));
  }

  // Request-level metrics
  {
    auto mc = MetricsCollectorFactory::get_default();

    // total requests
    auto total = std::dynamic_pointer_cast<CounterMetric>(mc->get_metric("agentflow.requests.total"));
    if (!total) total = mc->create_counter("agentflow.requests.total", "Total traced requests completed");
    total->increment();

    // error requests
    bool has_error = false;
    for (const auto& sp : sess.spans) {
      if (sp.status == SpanStatus::ERROR) { has_error = true; break; }
    }
    if (has_error) {
      auto errs = std::dynamic_pointer_cast<CounterMetric>(mc->get_metric("agentflow.requests.errors"));
      if (!errs) errs = mc->create_counter("agentflow.requests.errors", "Traced requests with at least one error span");
      errs->increment();
    }

    // request duration histogram
    if (sess.start_us > 0 && sess.end_us > sess.start_us) {
      double dur_ms = static_cast<double>(sess.end_us - sess.start_us) / 1000.0;
      auto hist = std::dynamic_pointer_cast<HistogramMetric>(mc->get_metric("agentflow.request.duration_ms"));
      if (!hist) {
        hist = mc->create_histogram("agentflow.request.duration_ms",
                                    "End-to-end request duration (ms)",
                                    {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000});
      }
      hist->record(dur_ms);
    }
  }

  const std::string task_id = sess.task_id;
  // Flush spans to NDJSON + metrics snapshot
  flush_session_internal(sess);

  // Move to completed store
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  completed_[task_id].push_back(std::move(sess));
}

TraceSession* TraceManager::get_session(const std::string& trace_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = active_sessions_.find(trace_id);
  if (it != active_sessions_.end()) return &it->second;
  return nullptr;
}

// ============================================================================
// Data access
// ============================================================================
std::vector<TraceSession> TraceManager::list_sessions(const std::string& task_id) const {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = completed_.find(task_id);
  if (it == completed_.end()) return {};
  return it->second;
}

std::vector<TraceSpan> TraceManager::get_spans(const std::string& task_id) const {
  std::vector<TraceSpan> result;
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = completed_.find(task_id);
  if (it == completed_.end()) return result;
  for (const auto& sess : it->second) {
    for (const auto& span : sess.spans) result.push_back(span);
  }
  return result;
}

// ============================================================================
// Metrics bridge
// ============================================================================
void TraceManager::feed_metrics(const TraceSpan& span) {
  auto mc = MetricsCollectorFactory::get_default();

  if (span.kind == SpanKind::NODE) {
    auto cnt = std::dynamic_pointer_cast<CounterMetric>(mc->get_metric("agentflow.nodes.executed"));
    if (!cnt) cnt = mc->create_counter("agentflow.nodes.executed", "Total node executions traced");
    cnt->increment();

    if (span.status == SpanStatus::ERROR) {
      auto errs = std::dynamic_pointer_cast<CounterMetric>(mc->get_metric("agentflow.nodes.errors"));
      if (!errs) errs = mc->create_counter("agentflow.nodes.errors", "Total node execution errors traced");
      errs->increment();
    }

    if (span.duration_us() > 0) {
      auto hist = std::dynamic_pointer_cast<HistogramMetric>(mc->get_metric("agentflow.node.duration_ms"));
      if (!hist) {
        hist = mc->create_histogram("agentflow.node.duration_ms",
                                    "Node execution duration (ms)",
                                    {0.5, 1, 2, 5, 10, 25, 50, 100, 250, 500, 1000});
      }
      hist->record(static_cast<double>(span.duration_us()) / 1000.0);
    }

  } else if (span.kind == SpanKind::WORKFLOW) {
    auto cnt = std::dynamic_pointer_cast<CounterMetric>(mc->get_metric("agentflow.workflows.executed"));
    if (!cnt) cnt = mc->create_counter("agentflow.workflows.executed", "Total workflow executions traced");
    cnt->increment();

    if (span.duration_us() > 0) {
      auto hist = std::dynamic_pointer_cast<HistogramMetric>(mc->get_metric("agentflow.workflow.duration_ms"));
      if (!hist) {
        hist = mc->create_histogram("agentflow.workflow.duration_ms",
                                    "Workflow execution duration (ms)",
                                    {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000});
      }
      hist->record(static_cast<double>(span.duration_us()) / 1000.0);
    }
  }
}

void TraceManager::flush_metrics_snapshot(const std::string& dir) {
  try {
    std::filesystem::create_directories(dir);
    std::string path = dir + "/metrics.json";
    auto mc = MetricsCollectorFactory::get_default();
    std::string json = mc->export_json();
    // Overwrite: metrics.json is a snapshot of current totals
    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) f << json;
  } catch (...) {}
}

// ============================================================================
// Persistence
// ============================================================================
void TraceManager::flush_session_internal(const TraceSession& sess) {
  std::string dir = output_dir_;
  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(sess.task_id);
    if (it != tasks_.end() && !it->second.output_dir.empty()) {
      dir = it->second.output_dir;
    }
  }

  try {
    std::filesystem::create_directories(dir);
    std::string path = dir + "/task_" + sess.task_id + ".ndjson";
    std::ofstream f(path, std::ios::app);
    if (!f.is_open()) return;
    for (const auto& span : sess.spans) {
      f << TraceExporter::span_to_ndjson(span) << "\n";
    }
  } catch (...) {}

  // Write updated metrics snapshot alongside the NDJSON
  flush_metrics_snapshot(dir);
}

bool TraceManager::flush_session(const std::string& trace_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = active_sessions_.find(trace_id);
  if (it == active_sessions_.end()) return false;
  flush_session_internal(it->second);
  return true;
}

void TraceManager::flush_task(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = completed_.find(task_id);
  if (it == completed_.end()) return;
  for (const auto& sess : it->second) {
    flush_session_internal(sess);
  }
}

}  // namespace trace
}  // namespace runtime
}  // namespace agentflow
