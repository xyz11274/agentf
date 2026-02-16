#ifndef AGENTFLOW_BASIC_LOGGER_H
#define AGENTFLOW_BASIC_LOGGER_H

#include <string>
#include <iostream>
#include <sstream>
#include <chrono>

namespace agentflow {

/**
 * Simple logger for AgentFlow system
 * Supports logging levels: DEBUG, INFO, WARN, ERROR
 */
class Logger {
public:
    enum class Level {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        ERROR = 3,
    };
    
    static Logger& instance();
    
    void set_level(Level level) { level_ = level; }
    Level get_level() const { return level_; }
    
    void log(Level level, const std::string& message);
    
    // Convenience methods
    void debug(const std::string& msg) { log(Level::DEBUG, msg); }
    void info(const std::string& msg) { log(Level::INFO, msg); }
    void warn(const std::string& msg) { log(Level::WARN, msg); }
    void error(const std::string& msg) { log(Level::ERROR, msg); }
    
    template<typename... Args>
    void debug_f(const char* fmt, Args... args) {
        log_formatted(Level::DEBUG, fmt, args...);
    }

    template<typename... Args>
    void info_f(const char* fmt, Args... args) {
        log_formatted(Level::INFO, fmt, args...);
    }

    template<typename... Args>
    void warn_f(const char* fmt, Args... args) {
        log_formatted(Level::WARN, fmt, args...);
    }

    template<typename... Args>
    void error_f(const char* fmt, Args... args) {
        log_formatted(Level::ERROR, fmt, args...);
    }
    
private:
    Logger() = default;
    Level level_ = Level::INFO;
    
    std::string get_timestamp() const;
    std::string get_level_string(Level level) const;
    
    template<typename... Args>
    void log_formatted(Level level, const char* fmt, Args... args) {
        std::ostringstream oss;
        format_string(oss, fmt, args...);
        log(level, oss.str());
    }
    
    template<typename T>
    void format_arg(std::ostringstream& oss, T val) {
        oss << val;
    }
    
    template<typename T, typename... Args>
    void format_string(std::ostringstream& oss, const char* fmt, T first, Args... rest) {
        while (*fmt) {
            if (*fmt == '%' && *(fmt + 1) == 's') {
                format_arg(oss, first);
                fmt += 2;
                format_string(oss, fmt, rest...);
                return;
            }
            oss << *fmt++;
        }
    }
    
    void format_string(std::ostringstream& oss, const char* fmt) {
        oss << fmt;
    }
};

} // namespace agentflow

#endif // AGENTFLOW_BASIC_LOGGER_H
