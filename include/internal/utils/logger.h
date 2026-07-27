#ifndef AGENTFLOW_BASIC_LOGGER_H
#define AGENTFLOW_BASIC_LOGGER_H

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "utils/log_sink.h"

namespace agentflow {

/**
 * Simple logger for AgentFlow system
 * Supports logging levels: DEBUG, INFO, WARN, ERROR
 * Log rotation: max file size + keep N backups
 */
class Logger {
public:
  enum class Level {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
  };

  static Logger& instance();

  void  set_level(Level level) { level_ = level; }
  Level get_level() const { return level_; }

  void set_sink(std::unique_ptr<ILogSink> sink);
  void set_log_file(const std::string& path);
  void set_log_rotation(int64_t max_bytes, int max_backups);
  void flush();
  void log(Level level, const std::string& message);

  // Convenience methods
  void debug(const std::string& msg) { log(Level::DEBUG, msg); }
  void info(const std::string& msg) { log(Level::INFO, msg); }
  void warn(const std::string& msg) { log(Level::WARN, msg); }
  void error(const std::string& msg) { log(Level::ERROR, msg); }

  template <typename... Args>
  void debug_f(const char* fmt, Args... args) {
    log_formatted(Level::DEBUG, fmt, args...);
  }

  template <typename... Args>
  void info_f(const char* fmt, Args... args) {
    log_formatted(Level::INFO, fmt, args...);
  }

  template <typename... Args>
  void warn_f(const char* fmt, Args... args) {
    log_formatted(Level::WARN, fmt, args...);
  }

  template <typename... Args>
  void error_f(const char* fmt, Args... args) {
    log_formatted(Level::ERROR, fmt, args...);
  }

private:
  Logger()     = default;
  Level level_ = Level::INFO;

  std::unique_ptr<ILogSink> sink_;
  std::ofstream             log_file_;
  std::mutex                mutex_;
  std::string               log_path_;          // 当前日志文件路径
  int64_t                   max_file_bytes_ = 0;  // 0 = 不轮转
  int                       max_backups_    = 3;
  bool                      rotate_pending_ = false;  // 阶段 1 标记，阶段 2 执行

  void rotate_log();                  // 阶段 1：锁内检查 file_size 并标记
  void rotate_exec_unlocked();        // 阶段 2：执行 fs::rename/remove 并重开文件
  std::string get_timestamp() const;
  std::string get_level_string(Level level) const;

  template <typename... Args>
  void log_formatted(Level level, const char* fmt, Args... args) {
    std::ostringstream oss;
    format_string(oss, fmt, args...);
    log(level, oss.str());
  }

  template <typename T>
  void format_arg(std::ostringstream& oss, T val) {
    oss << val;
  }

  template <typename T, typename... Args>
  void format_string(std::ostringstream& oss, const char* fmt, T first, Args... rest) {
    while(*fmt) {
      if(*fmt == '%') {
        const char* next = fmt + 1;
        // Skip format modifiers (l, ll, z, h, etc.)
        while(*next == 'l' || *next == 'h' || *next == 'z' || *next == 'j' || *next == 't') {
          next++;
        }
        // Check for format specifiers
        if(*next == 'd' || *next == 'i' || *next == 'u' || *next == 'x' || *next == 'X' ||
           *next == 'o' || *next == 'f' || *next == 'F' || *next == 'e' || *next == 'E' ||
           *next == 'g' || *next == 'G' || *next == 'a' || *next == 'A' || *next == 'c' ||
           *next == 's' || *next == 'p' || *next == 'n') {
          format_arg(oss, first);
          fmt = next + 1;
          format_string(oss, fmt, rest...);
          return;
        }
      }
      oss << *fmt++;
    }
  }

  void format_string(std::ostringstream& oss, const char* fmt) { oss << fmt; }
};

}  // namespace agentflow

#endif  // AGENTFLOW_BASIC_LOGGER_H
