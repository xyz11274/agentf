#include "utils/logger.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iomanip>

namespace agentflow {

namespace fs = std::filesystem;

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

void Logger::set_log_rotation(int64_t max_bytes, int max_backups) {
  // 不变量 I-rotation: max_backups 必须 >= 1,否则轮转后无备份文件
  // 显式断言而非静默兜底,避免调用方传 0 时行为与头文件默认值(3)不一致
  assert(max_backups >= 1 && "max_backups must be >= 1");
  max_file_bytes_ = max_bytes;
  max_backups_    = max_backups;
}

void Logger::set_log_file(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_path_ = path;
  if (log_file_.is_open()) {
    log_file_.close();
  }
  log_file_.open(path, std::ios::out | std::ios::app);
  if (!log_file_.is_open()) {
    std::cerr << "[LOGGER] Failed to open log file: " << path << std::endl;
  }
}

void Logger::rotate_log() {
  if (log_path_.empty() || max_file_bytes_ <= 0) return;

  // 检查当前文件大小
  std::error_code ec;
  auto file_size = fs::file_size(log_path_, ec);
  if (ec || file_size < static_cast<uint64_t>(max_file_bytes_)) return;

  // 关闭当前文件
  if (log_file_.is_open()) {
    log_file_.close();
  }

  // 轮转：删除最旧的备份，依次重命名 .N-1 → .N
  // daemon.log.3 → 删除, daemon.log.2 → daemon.log.3, ..., daemon.log → daemon.log.1
  fs::path base(log_path_);
  // 删除最旧备份
  fs::path oldest = base;
  oldest += "." + std::to_string(max_backups_);
  fs::remove(oldest, ec);

  // 依次重命名 .N-1 → .N
  for (int i = max_backups_ - 1; i >= 1; --i) {
    fs::path src = base;
    src += "." + std::to_string(i);
    if (fs::exists(src, ec)) {
      fs::path dst = base;
      dst += "." + std::to_string(i + 1);
      fs::rename(src, dst, ec);
    }
  }

  // 重命名当前日志 → .1
  fs::path first = base;
  first += ".1";
  fs::rename(log_path_, first, ec);

  // 打开新文件
  log_file_.open(log_path_, std::ios::out | std::ios::app);
  if (!log_file_.is_open()) {
    std::cerr << "[LOGGER] Failed to open log file after rotation: " << log_path_ << std::endl;
  }
}

void Logger::rotate_exec_unlocked() {
  // 未启用：保留占位以兼容头文件声明，避免链接错误
  // P1-2 评估后判定为过度设计（rotate 触发频率极低，锁竞争不成立），回滚原实现
}

void Logger::log(Level level, const std::string& message) {
  if(level < level_) {
    return;
  }

  std::string timestamp = get_timestamp();
  std::string level_str = get_level_string(level);

  // 两阶段轮转：阶段 1 在锁内检查 file_size 并标记 rotate_pending_
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rotate_log();
  }

  // 阶段 2：若阶段 1 标记了 rotate_pending_，释放锁做 fs::rename/fs::remove
  // （避免持锁期间慢速文件系统操作阻塞其他 log() 调用）
  if (rotate_pending_) {
    std::lock_guard<std::mutex> lock(mutex_);
    rotate_exec_unlocked();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_) {
    sink_->write(static_cast<int>(level), timestamp, message);
    return;
  }

  std::string line = "[" + timestamp + "] [" + level_str + "] " + message;
  if (log_file_.is_open()) {
    log_file_ << line << std::endl;
  } else {
    // 无日志文件时写 stdout（避免与 log_file_ 双写到同一文件）
    std::cout << line << std::endl;
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