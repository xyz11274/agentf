// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// trace_exporter.h — Export trace data to JSON or CSV.

#ifndef AGENTFLOW_RUNTIME_TRACE_EXPORTER_H
#define AGENTFLOW_RUNTIME_TRACE_EXPORTER_H

#include <string>
#include <vector>

#include "runtime/trace/trace_types.h"

namespace agentflow {
namespace runtime {
namespace trace {

class TraceExporter {
public:
  // Export a list of spans to a JSON array string
  static std::string spans_to_json(const std::vector<TraceSpan>& spans);

  // Export a list of sessions (with nested spans) to a JSON array string
  static std::string sessions_to_json(const std::vector<TraceSession>& sessions);

  // Export spans to CSV (one span per row)
  // Columns: trace_id,span_id,parent_span_id,kind,name,thread_id,
  //          start_us,end_us,duration_us,status,error_msg,tags
  static std::string spans_to_csv(const std::vector<TraceSpan>& spans);

  // Convenience: export to file; returns true on success
  static bool export_json_to_file(const std::vector<TraceSession>& sessions,
                                  const std::string& path);
  static bool export_csv_to_file(const std::vector<TraceSpan>& spans,
                                 const std::string& path);

  // Single-span NDJSON line (used for streaming file output)
  static std::string span_to_ndjson(const TraceSpan& span);

private:
  static std::string escape_json_str(const std::string& s);
  static std::string escape_csv_field(const std::string& s);
};

}  // namespace trace
}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_TRACE_EXPORTER_H
