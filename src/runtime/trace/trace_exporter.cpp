// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "runtime/trace/trace_exporter.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace agentflow {
namespace runtime {
namespace trace {

// ============================================================================
// Helpers
// ============================================================================
std::string TraceExporter::escape_json_str(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

std::string TraceExporter::escape_csv_field(const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else          out += c;
  }
  out += '"';
  return out;
}

// ============================================================================
// Single span → NDJSON line
// ============================================================================
std::string TraceExporter::span_to_ndjson(const TraceSpan& span) {
  std::ostringstream o;
  o << "{"
    << "\"trace_id\":\""    << escape_json_str(span.trace_id)        << "\","
    << "\"span_id\":\""     << escape_json_str(span.span_id)         << "\","
    << "\"parent_id\":\""   << escape_json_str(span.parent_span_id)  << "\","
    << "\"kind\":\""        << span_kind_name(span.kind)             << "\","
    << "\"name\":\""        << escape_json_str(span.name)            << "\","
    << "\"thread_id\":"     << span.thread_id                        << ","
    << "\"start_us\":"      << span.start_us                         << ","
    << "\"end_us\":"        << span.end_us                           << ","
    << "\"duration_us\":"   << span.duration_us()                    << ","
    << "\"status\":\""      << span_status_name(span.status)         << "\","
    << "\"error_msg\":\""   << escape_json_str(span.error_msg)       << "\","
    << "\"cpu_percent\":"   << span.cpu_percent                      << ","
    << "\"rss_kb\":"        << span.rss_kb                           << ","
    << "\"tags\":{";
  bool first_tag = true;
  for (const auto& kv : span.tags) {
    if (!first_tag) o << ",";
    o << "\"" << escape_json_str(kv.first) << "\":\""
      << escape_json_str(kv.second) << "\"";
    first_tag = false;
  }
  o << "},\"events\":[";
  bool first_ev = true;
  for (const auto& ev : span.events) {
    if (!first_ev) o << ",";
    o << "{\"t\":" << ev.timestamp_us
      << ",\"name\":\"" << escape_json_str(ev.name) << "\""
      << ",\"attrs\":{";
    bool first_a = true;
    for (const auto& a : ev.attributes) {
      if (!first_a) o << ",";
      o << "\"" << escape_json_str(a.first) << "\":\""
        << escape_json_str(a.second) << "\"";
      first_a = false;
    }
    o << "}}";
    first_ev = false;
  }
  o << "]}";
  return o.str();
}

// ============================================================================
// spans → JSON array
// ============================================================================
std::string TraceExporter::spans_to_json(const std::vector<TraceSpan>& spans) {
  std::ostringstream o;
  o << "[";
  bool first = true;
  for (const auto& s : spans) {
    if (!first) o << ",";
    o << span_to_ndjson(s);
    first = false;
  }
  o << "]";
  return o.str();
}

// ============================================================================
// sessions → JSON array
// ============================================================================
std::string TraceExporter::sessions_to_json(const std::vector<TraceSession>& sessions) {
  std::ostringstream o;
  o << "[";
  bool first_sess = true;
  for (const auto& sess : sessions) {
    if (!first_sess) o << ",";
    o << "{"
      << "\"trace_id\":\""   << escape_json_str(sess.trace_id)   << "\","
      << "\"task_id\":\""    << escape_json_str(sess.task_id)    << "\","
      << "\"request_id\":\"" << escape_json_str(sess.request_id) << "\","
      << "\"user_id\":\""    << escape_json_str(sess.user_id)    << "\","
      << "\"client_id\":\""  << escape_json_str(sess.client_id)  << "\","
      << "\"session_id\":\"" << escape_json_str(sess.session_id) << "\","
      << "\"start_us\":"     << sess.start_us                    << ","
      << "\"end_us\":"       << sess.end_us                      << ","
      << "\"spans\":"        << spans_to_json(sess.spans)
      << "}";
    first_sess = false;
  }
  o << "]";
  return o.str();
}

// ============================================================================
// spans → CSV
// ============================================================================
std::string TraceExporter::spans_to_csv(const std::vector<TraceSpan>& spans) {
  std::ostringstream o;
  o << "trace_id,span_id,parent_span_id,kind,name,thread_id,"
       "start_us,end_us,duration_us,status,error_msg,tags\n";
  for (const auto& s : spans) {
    // Flatten tags to k=v;k=v
    std::string tags_str;
    for (const auto& kv : s.tags) {
      if (!tags_str.empty()) tags_str += ";";
      tags_str += kv.first + "=" + kv.second;
    }
    o << escape_csv_field(s.trace_id)       << ","
      << escape_csv_field(s.span_id)        << ","
      << escape_csv_field(s.parent_span_id) << ","
      << escape_csv_field(span_kind_name(s.kind)) << ","
      << escape_csv_field(s.name)           << ","
      << s.thread_id                        << ","
      << s.start_us                         << ","
      << s.end_us                           << ","
      << s.duration_us()                    << ","
      << escape_csv_field(span_status_name(s.status)) << ","
      << escape_csv_field(s.error_msg)      << ","
      << escape_csv_field(tags_str)         << "\n";
  }
  return o.str();
}

// ============================================================================
// File exports
// ============================================================================
bool TraceExporter::export_json_to_file(const std::vector<TraceSession>& sessions,
                                        const std::string& path) {
  try {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << sessions_to_json(sessions);
    return true;
  } catch (...) {
    return false;
  }
}

bool TraceExporter::export_csv_to_file(const std::vector<TraceSpan>& spans,
                                       const std::string& path) {
  try {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << spans_to_csv(spans);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace trace
}  // namespace runtime
}  // namespace agentflow
