// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#ifndef AGENTFLOW_EXAMPLES_TUI_SUPPORT_H
#define AGENTFLOW_EXAMPLES_TUI_SUPPORT_H

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentflow::examples {

inline std::string trim_text(const std::string& input) {
  const auto start = input.find_first_not_of(" \t\r\n");
  if(start == std::string::npos) {
    return "";
  }
  const auto end = input.find_last_not_of(" \t\r\n");
  return input.substr(start, end - start + 1);
}

class TuiConsole {
 public:
  explicit TuiConsole(std::filesystem::path log_path) : log_path_(std::move(log_path)) {}

  ~TuiConsole() { close_handles_only(); }

  bool redirect_process_logs(std::string* error_message = nullptr) {
    if(active_) {
      return true;
    }
    std::filesystem::create_directories(log_path_.parent_path());
    std::fflush(stdout);
    std::fflush(stderr);
    saved_stdout_ = ::dup(STDOUT_FILENO);
    saved_stderr_ = ::dup(STDERR_FILENO);
    if(saved_stdout_ < 0 || saved_stderr_ < 0) {
      if(error_message != nullptr) {
        *error_message = std::string("dup failed: ") + std::strerror(errno);
      }
      restore();
      return false;
    }

    log_fd_ = ::open(log_path_.string().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if(log_fd_ < 0) {
      if(error_message != nullptr) {
        *error_message = std::string("open failed: ") + std::strerror(errno);
      }
      restore();
      return false;
    }

    if(::dup2(log_fd_, STDOUT_FILENO) < 0 || ::dup2(log_fd_, STDERR_FILENO) < 0) {
      if(error_message != nullptr) {
        *error_message = std::string("dup2 failed: ") + std::strerror(errno);
      }
      restore();
      return false;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    active_ = true;
    return true;
  }

  void write_line(const std::string& text) const { write_raw(text + "\n"); }

  void write_blank_line() const { write_raw("\n"); }

  void write_prefixed_lines(const std::string& prefix, const std::string& text,
                            const std::string& empty_fallback = "") const {
    size_t cursor = 0;
    bool printed = false;
    while(cursor <= text.size()) {
      size_t next = text.find('\n', cursor);
      const std::string chunk = next == std::string::npos ? text.substr(cursor)
                                                          : text.substr(cursor, next - cursor);
      if(!chunk.empty()) {
        write_line(prefix + chunk);
        printed = true;
      }
      if(next == std::string::npos) {
        break;
      }
      cursor = next + 1;
    }
    if(!printed && !empty_fallback.empty()) {
      write_line(prefix + empty_fallback);
    }
  }

  std::filesystem::path log_path() const { return log_path_; }

 private:
  void write_raw(std::string_view text) const {
    if(saved_stdout_ < 0) {
      return;
    }
    const char* data = text.data();
    size_t remaining = text.size();
    while(remaining > 0) {
      const ssize_t written = ::write(saved_stdout_, data, remaining);
      if(written <= 0) {
        break;
      }
      data += written;
      remaining -= static_cast<size_t>(written);
    }
  }

  void restore() {
    if(saved_stdout_ >= 0) {
      std::fflush(stdout);
      ::dup2(saved_stdout_, STDOUT_FILENO);
      ::close(saved_stdout_);
      saved_stdout_ = -1;
    }
    if(saved_stderr_ >= 0) {
      std::fflush(stderr);
      ::dup2(saved_stderr_, STDERR_FILENO);
      ::close(saved_stderr_);
      saved_stderr_ = -1;
    }
    if(log_fd_ >= 0) {
      ::close(log_fd_);
      log_fd_ = -1;
    }
    active_ = false;
  }

  void close_handles_only() {
    if(saved_stdout_ >= 0) {
      ::close(saved_stdout_);
      saved_stdout_ = -1;
    }
    if(saved_stderr_ >= 0) {
      ::close(saved_stderr_);
      saved_stderr_ = -1;
    }
    if(log_fd_ >= 0) {
      ::close(log_fd_);
      log_fd_ = -1;
    }
    active_ = false;
  }

  std::filesystem::path log_path_;
  int saved_stdout_ = -1;
  int saved_stderr_ = -1;
  int log_fd_ = -1;
  bool active_ = false;
};

}  // namespace agentflow::examples

#endif  // AGENTFLOW_EXAMPLES_TUI_SUPPORT_H
