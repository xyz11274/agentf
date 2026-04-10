// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// flow_trace.cpp — Flow trace logger implementation.

#include "runtime/flow_trace.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace agentflow {
namespace runtime {

// ============================================================================
// Time utilities
// ============================================================================

int64_t now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

std::string format_timestamp(int64_t timestamp_us) {
  using namespace std::chrono;

  auto time_point = system_clock::time_point(microseconds(timestamp_us));
  auto time_t_val = system_clock::to_time_t(time_point);

  // Get milliseconds part
  auto ms_part = duration_cast<milliseconds>(microseconds(timestamp_us % 1000000));

  std::tm tm_val;
  localtime_r(&time_t_val, &tm_val);

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_val);

  std::ostringstream oss;
  oss << buffer << "." << std::setfill('0') << std::setw(3) << ms_part.count();
  return oss.str();
}

// ============================================================================
// JSON escaping utility
// ============================================================================

std::string escape_json_string(const std::string& s) {
  std::ostringstream oss;
  for (char c : s) {
    switch (c) {
      case '"':
        oss << "\\\"";
        break;
      case '\\':
        oss << "\\\\";
        break;
      case '\b':
        oss << "\\b";
        break;
      case '\f':
        oss << "\\f";
        break;
      case '\n':
        oss << "\\n";
        break;
      case '\r':
        oss << "\\r";
        break;
      case '\t':
        oss << "\\t";
        break;
      default:
        if ('\x00' <= c && c <= '\x1f') {
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c);
        } else {
          oss << c;
        }
    }
  }
  return oss.str();
}

// ============================================================================
// FlowTraceEvent serialization
// ============================================================================

std::string to_json_line(const FlowTraceEvent& event) {
  std::ostringstream oss;

  oss << "{";

  // Time
  oss << "\"time\":\"" << escape_json_string(event.time) << "\"";

  // Type
  oss << ",\"type\":\"" << escape_json_string(event.type) << "\"";

  // Session and dialogue
  if (!event.session_id.empty()) {
    oss << ",\"session\":\"" << escape_json_string(event.session_id) << "\"";
  }
  if (!event.dialogue_id.empty()) {
    oss << ",\"dialogue\":\"" << escape_json_string(event.dialogue_id) << "\"";
  }

  // From and To
  oss << ",\"from\":\"" << escape_json_string(event.from_entity) << "\"";
  oss << ",\"to\":\"" << escape_json_string(event.to_entity) << "\"";

  // Event type
  oss << ",\"event\":\"" << escape_json_string(event.event_type) << "\"";

  // Optional inputs
  if (!event.inputs.empty()) {
    oss << ",\"inputs\":\"" << escape_json_string(event.inputs) << "\"";
  }

  // Optional outputs
  if (!event.outputs.empty()) {
    oss << ",\"outputs\":\"" << escape_json_string(event.outputs) << "\"";
  }

  // Optional labels (for correlation from external message.metadata["trace_labels"])
  if (!event.labels.empty()) {
    oss << ",\"labels\":\"" << escape_json_string(event.labels) << "\"";
  }

  // Duration (only for completion events)
  if (event.duration_us > 0) {
    oss << ",\"duration_us\":" << event.duration_us;
  }

  // Task ID (optional)
  if (!event.task_id.empty()) {
    oss << ",\"task_id\":\"" << escape_json_string(event.task_id) << "\"";
  }

  // Trace ID (optional)
  if (!event.trace_id.empty()) {
    oss << ",\"trace_id\":\"" << escape_json_string(event.trace_id) << "\"";
  }

  // Additional attributes
  if (!event.attributes.empty()) {
    oss << ",\"attrs\":{";
    bool first = true;
    for (const auto& kv : event.attributes) {
      if (!first) oss << ",";
      oss << "\"" << escape_json_string(kv.first) << "\":\""
          << escape_json_string(kv.second) << "\"";
      first = false;
    }
    oss << "}";
  }

  oss << "}";

  return oss.str();
}

// ============================================================================
// FlowTraceWriter implementation
// ============================================================================

FlowTraceWriter::~FlowTraceWriter() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (output_file_.is_open()) {
    output_file_.flush();
    output_file_.close();
  }
}

FlowTraceWriter& FlowTraceWriter::instance() {
  static FlowTraceWriter instance;

  // Check environment variable for output file on first access
  static bool initialized = false;
  if (!initialized) {
    const char* env_file = std::getenv("FLOW_TRACE_FILE");
    if (env_file && env_file[0] != '\0') {
      instance.set_output_file(env_file);
    }
    initialized = true;
  }

  return instance;
}

void FlowTraceWriter::set_output_file(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (output_file_.is_open()) {
    output_file_.close();
  }
  output_file_.open(path, std::ios::app);
  if (output_file_.is_open()) {
    file_enabled_ = true;
  } else {
    std::cerr << "[FlowTrace] Failed to open output file: " << path << std::endl;
    file_enabled_ = false;
  }
}

void FlowTraceWriter::set_stdout_enabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  stdout_enabled_ = enabled;
}

void FlowTraceWriter::set_enabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  enabled_ = enabled;
}

void FlowTraceWriter::write_event(const FlowTraceEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!enabled_) {
    return;
  }

  std::string json_line = to_json_line(event);

  if (stdout_enabled_) {
    std::cout << json_line << std::endl;
  }

  if (file_enabled_ && output_file_.is_open()) {
    output_file_ << json_line << std::endl;
  }
}

void FlowTraceWriter::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_enabled_ && output_file_.is_open()) {
    output_file_.flush();
  }
  if (stdout_enabled_) {
    std::cout.flush();
  }
}

// ============================================================================
// Helper functions
// ============================================================================

FlowTraceEvent make_flow_event(const std::string& type,
                                const std::string& session_id,
                                const std::string& dialogue_id,
                                const std::string& from_entity,
                                const std::string& to_entity,
                                FlowEventType event_type) {
  FlowTraceEvent event;
  event.timestamp_us = now_us();
  event.time         = format_timestamp(event.timestamp_us);
  event.type         = type;
  event.session_id   = session_id;
  event.dialogue_id  = dialogue_id;
  event.from_entity  = from_entity;
  event.to_entity    = to_entity;
  event.event_type   = flow_event_type_name(event_type);
  return event;
}

FlowTraceEvent make_task_event(const std::string& session_id,
                                const std::string& dialogue_id,
                                const std::string& from_entity,
                                const std::string& to_entity,
                                FlowEventType event_type) {
  return make_flow_event("task-exec", session_id, dialogue_id, from_entity, to_entity,
                          event_type);
}

FlowTraceEvent make_workflow_event(const std::string& session_id,
                                    const std::string& dialogue_id,
                                    const std::string& from_entity,
                                    const std::string& to_entity,
                                    FlowEventType event_type) {
  return make_flow_event("workflow-exec", session_id, dialogue_id, from_entity,
                          to_entity, event_type);
}

FlowTraceEvent make_node_event(const std::string& session_id,
                                const std::string& dialogue_id,
                                const std::string& from_entity,
                                const std::string& to_entity,
                                FlowEventType event_type) {
  return make_flow_event("node-exec", session_id, dialogue_id, from_entity, to_entity,
                          event_type);
}

}  // namespace runtime
}  // namespace agentflow