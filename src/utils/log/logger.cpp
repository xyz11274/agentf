#include "utils/logger.h"

#include <iomanip>

namespace agentflow {

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::log(Level level, const std::string& message) {
  if(level < level_) {
    return;
  }

  std::string timestamp = get_timestamp();
  std::string level_str = get_level_string(level);

  std::cout << "[" << timestamp << "] [" << level_str << "] " << message << std::endl;
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
