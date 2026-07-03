#include "utils/logger.h"

#include <iomanip>

namespace agentflow {

Logger& Logger::instance() {
  // Logger is used by other process-lifetime singletons from their
  // destructors. Intentionally keep it alive until process termination to
  // avoid cross-translation-unit static destruction order races.
  static Logger* logger = new Logger();
  return *logger;
}

void Logger::set_sink(std::unique_ptr<ILogSink> sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_) sink_->flush();
  sink_ = std::move(sink);
}

void Logger::set_log_file(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (log_file_.is_open()) {
    log_file_.close();
  }
  log_file_.open(path, std::ios::out | std::ios::app);
  if (!log_file_.is_open()) {
    std::cerr << "[LOGGER] Failed to open log file: " << path << std::endl;
  }
}

void Logger::log(Level level, const std::string& message) {
  if(level < level_) {
    return;
  }

  std::string timestamp = get_timestamp();
  std::string level_str = get_level_string(level);

  std::lock_guard<std::mutex> lock(mutex_);

  if (sink_) {
    sink_->write(static_cast<int>(level), timestamp, message);
    return;
  }

  std::string line = "[" + timestamp + "] [" + level_str + "] " + message;
  std::cout << line << std::endl;
  if (log_file_.is_open()) {
    log_file_ << line << std::endl;
  }
}

void Logger::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_) {
    sink_->flush();
  } else {
    std::cout.flush();
    if (log_file_.is_open()) {
      log_file_.flush();
    }
  }
}

std::string Logger::get_timestamp() const {
  auto now        = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::ostringstream oss;
  oss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
  oss << "." << std::setfill('0') << std::setw(3) << ms.count();

  return oss.str();
}

std::string Logger::get_level_string(Level level) const {
  switch(level) {
    case Level::DEBUG:
      return "DEBUG";
    case Level::INFO:
      return "INFO";
    case Level::WARN:
      return "WARN";
    case Level::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

}  // namespace agentflow
