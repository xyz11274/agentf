#ifndef AGENTFLOW_RUNTIME_HOT_RELOAD_H
#define AGENTFLOW_RUNTIME_HOT_RELOAD_H

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace runtime {

/**
 * Executable file types that support hot reload
 */
enum class ExecutableType {
  UNKNOWN = 0,
  SO_LIBRARY,     // Shared object library (.so, .dylib)
  JAR_FILE,       // Java Archive (.jar)
  SHELL_SCRIPT,   // Shell script (.sh, .bash)
  PYTHON_SCRIPT,  // Python script (.py)
  EXECUTABLE      // Executable binary
};

/**
 * Convert ExecutableType to string
 */
std::string executable_type_to_string(ExecutableType type);

/**
 * Detect executable type from file extension
 */
ExecutableType detect_executable_type(const std::string& file_path);

/**
 * Version information for executable files
 */
struct ExecutableVersion {
  std::string                           version_id;  // Unique version identifier
  std::string                           file_path;   // Path to the executable file
  std::string                           checksum;    // MD5/SHA256 checksum
  int64_t                               file_size;   // File size in bytes
  std::chrono::system_clock::time_point modified_time;
  std::string                           metadata;   // Additional metadata (JSON format)
  bool                                  is_active;  // Whether this version is currently in use

  ExecutableVersion() : file_size(0), is_active(false) {}
};

/**
 * Hot reload configuration
 */
struct HotReloadConfig {
  bool        enabled                = true;  // Enable hot reload
  bool        auto_reload            = true;  // Automatically reload on file change
  int         max_versions           = 5;     // Maximum number of versions to keep
  int         check_interval_ms      = 1000;  // File check interval (milliseconds)
  bool        validate_before_reload = true;  // Validate file before reloading
  bool        backup_before_reload   = true;  // Create backup before reloading
  std::string backup_dir;                     // Directory for backups
  int         reload_timeout_ms = 30000;      // Timeout for reload operation

  HotReloadConfig() = default;
};

/**
 * File change event types
 */
enum class FileChangeEvent { CREATED = 0, MODIFIED = 1, DELETED = 2, MOVED = 3 };

/**
 * File change event
 */
struct FileChange {
  std::string                           file_path;
  FileChangeEvent                       event_type;
  std::chrono::system_clock::time_point timestamp;

  FileChange()
      : event_type(FileChangeEvent::MODIFIED), timestamp(std::chrono::system_clock::now()) {}
};

/**
 * Callback for file change notifications
 */
using FileChangeCallback = std::function<void(const FileChange&)>;

/**
 * Hot reload status
 */
enum class HotReloadStatus {
  IDLE      = 0,  // Not active
  WATCHING  = 1,  // Monitoring files
  RELOADING = 2,  // Reloading in progress
  ERROR     = 3   // Error occurred
};

/**
 * Reload result
 */
struct ReloadResult {
  bool                      success;
  std::string               version_id;
  std::string               message;
  std::chrono::milliseconds duration;

  ReloadResult() : success(false), duration(0) {}
};

/**
 * Callback for reload completion
 */
using ReloadCallback = std::function<void(const ReloadResult&)>;

/**
 * HotReloadManager manages executable code hot reload functionality
 */
class HotReloadManager {
public:
  HotReloadManager();
  ~HotReloadManager();

  // Disable copy and move
  HotReloadManager(const HotReloadManager&)            = delete;
  HotReloadManager& operator=(const HotReloadManager&) = delete;
  HotReloadManager(HotReloadManager&&)                 = delete;
  HotReloadManager& operator=(HotReloadManager&&)      = delete;

  // Configuration
  void                   set_config(const HotReloadConfig& config);
  const HotReloadConfig& get_config() const { return config_; }

  // File monitoring
  bool start_watching(const std::string& file_path);
  bool stop_watching(const std::string& file_path);
  bool is_watching(const std::string& file_path) const;

  // Register file change callback
  void register_file_change_callback(FileChangeCallback callback);

  // Version management
  std::shared_ptr<ExecutableVersion> get_current_version(const std::string& file_path) const;
  std::vector<std::shared_ptr<ExecutableVersion>> get_all_versions(
      const std::string& file_path) const;
  bool rollback_to_version(const std::string& file_path, const std::string& version_id);

  // Hot reload
  ReloadResult reload(const std::string& file_path);
  void         reload_async(const std::string& file_path, ReloadCallback callback);

  // Status
  HotReloadStatus get_status() const { return status_; }
  size_t          get_watched_files_count() const;

private:
  HotReloadConfig config_;
  HotReloadStatus status_;

  // File monitoring
  struct FileWatcherImpl;
  std::unique_ptr<FileWatcherImpl> watcher_impl_;

  // Version management
  struct VersionManagerImpl;
  std::unique_ptr<VersionManagerImpl> version_manager_impl_;

  // Callbacks
  std::vector<FileChangeCallback> file_change_callbacks_;

  // Internal methods
  void        on_file_changed(const FileChange& change);
  std::string calculate_checksum(const std::string& file_path) const;
  bool        validate_file(const std::string& file_path) const;
  bool        create_backup(const std::string& file_path, const std::string& backup_path) const;
};

/**
 * Get the global hot reload manager instance
 */
HotReloadManager& global_hot_reload_manager();

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_HOT_RELOAD_H
