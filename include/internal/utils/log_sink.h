// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// ILogSink — abstract log sink interface for injecting external log components.
//
// Usage:
//   1. Implement ILogSink with your external logger (spdlog, glog, etc.)
//   2. Call Logger::instance().set_sink(std::make_unique<YourSink>(...))
//   3. All Logger::{info,debug,warn,error}{,_f} calls automatically delegate

#ifndef AGENTFLOW_UTILS_LOG_SINK_H
#define AGENTFLOW_UTILS_LOG_SINK_H

#include <string>

namespace agentflow {

class ILogSink {
public:
  ILogSink() = default;
  ILogSink(const ILogSink&) = delete;
  ILogSink& operator=(const ILogSink&) = delete;
  ILogSink(ILogSink&&) = delete;
  ILogSink& operator=(ILogSink&&) = delete;
  virtual ~ILogSink() = default;

  // Write a single log record.
  // The caller (Logger) has already:
  //   - Checked the log level threshold
  //   - Formatted the timestamp string
  // level: the original Level enum value (DEBUG=0, INFO=1, WARN=2, ERROR=3)
  // timestamp: "YYYY-MM-DD HH:MM:SS.mmm" formatted string
  // message: the final log message (printf-style arguments already substituted)
  virtual void write(int level, const std::string& timestamp,
                     const std::string& message) = 0;

  // Flush buffered output. Called by Logger::flush() and on shutdown.
  virtual void flush() = 0;
};

}  // namespace agentflow

#endif  // AGENTFLOW_UTILS_LOG_SINK_H
