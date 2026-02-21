#ifndef AGENTFLOW_RUNTIME_CODE_LOADER_H
#define AGENTFLOW_RUNTIME_CODE_LOADER_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>

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
 * Code execution result
 */
struct ExecutionResult {
  bool        success;
  std::string stdout;
  std::string stderr;
  int         exit_code;
  std::string error_message;
  int64_t     duration_ms;

  ExecutionResult() : success(false), exit_code(-1), duration_ms(0) {}
};

/**
 * Execution configuration
 */
struct ExecutionConfig {
  std::string              working_dir;         // Working directory
  int                      timeout_ms = 30000;  // Execution timeout
  std::vector<std::string> env_vars;            // Additional environment variables
  DataMap                  inputs;              // Input data (use DataMap from common.h)

  ExecutionConfig() = default;
};

/**
 * SO library function signature
 */
using SoFunction = void (*)(const DataMap& inputs, DataMap& outputs);

/**
 * CodeLoader handles loading and executing different types of executable code
 */
class CodeLoader {
public:
  CodeLoader();
  ~CodeLoader();

  // Disable copy and move
  CodeLoader(const CodeLoader&)            = delete;
  CodeLoader& operator=(const CodeLoader&) = delete;
  CodeLoader(CodeLoader&&)                 = delete;
  CodeLoader& operator=(CodeLoader&&)      = delete;

  /**
   * Load and execute a shared object library
   * @param file_path Path to the .so/.dylib file
   * @param function_name Name of the function to call
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult load_and_execute_so(const std::string&     file_path,
                                      const std::string&     function_name,
                                      const ExecutionConfig& config = ExecutionConfig());

  /**
   * Execute a JAR file
   * @param jar_path Path to the .jar file
   * @param main_class Main class to execute
   * @param args Command line arguments
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult execute_jar(const std::string& jar_path, const std::string& main_class,
                              const std::vector<std::string>& args   = {},
                              const ExecutionConfig&          config = ExecutionConfig());

  /**
   * Execute a shell script
   * @param script_path Path to the shell script
   * @param args Command line arguments
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult execute_shell_script(const std::string&              script_path,
                                       const std::vector<std::string>& args   = {},
                                       const ExecutionConfig&          config = ExecutionConfig());

  /**
   * Execute a Python script
   * @param script_path Path to the Python script
   * @param args Command line arguments
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult execute_python_script(const std::string&              script_path,
                                        const std::vector<std::string>& args   = {},
                                        const ExecutionConfig&          config = ExecutionConfig());

  /**
   * Generic execute method that auto-detects file type
   * @param file_path Path to the executable file
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult execute(const std::string&     file_path,
                          const ExecutionConfig& config = ExecutionConfig());

  /**
   * Reload code for hot reload
   * @param file_path Path to the file to reload
   * @return Reload result
   */
  ReloadResult reload_code(const std::string& file_path);

  // Hot Reload Management Methods

  /**
   * Set hot reload configuration
   * @param config Configuration
   */
  void set_hot_reload_config(const HotReloadConfig& config);

  /**
   * Get hot reload configuration
   * @return Configuration
   */
  const HotReloadConfig& get_hot_reload_config() const;

  /**
   * Start watching a file for changes
   * @param file_path Path to file
   * @return true if started successfully
   */
  bool start_watching(const std::string& file_path);

  /**
   * Stop watching a file
   * @param file_path Path to file
   * @return true if stopped successfully
   */
  bool stop_watching(const std::string& file_path);

  /**
   * Check if file is being watched
   * @param file_path Path to file
   * @return true if watched
   */
  bool is_watching(const std::string& file_path) const;

  /**
   * Register callback for file changes
   * @param callback Callback function
   */
  void register_file_change_callback(FileChangeCallback callback);

  /**
   * Get current version of a file
   * @param file_path Path to file
   * @return Shared pointer to version info
   */
  std::shared_ptr<ExecutableVersion> get_current_version(const std::string& file_path) const;

  /**
   * Get all versions of a file
   * @param file_path Path to file
   * @return Vector of version info
   */
  std::vector<std::shared_ptr<ExecutableVersion>> get_all_versions(const std::string& file_path) const;

  /**
   * Rollback to a specific version
   * @param file_path Path to file
   * @param version_id Version ID to rollback to
   * @return true if successful
   */
  bool rollback_to_version(const std::string& file_path, const std::string& version_id);

  /**
   * Reload code asynchronously
   * @param file_path Path to file
   * @param callback Callback on completion
   */
  void reload_code_async(const std::string& file_path, ReloadCallback callback);

  /**
   * Get hot reload status
   * @return Status
   */
  HotReloadStatus get_hot_reload_status() const;

  /**
   * Get number of watched files
   * @return Count
   */
  size_t get_watched_files_count() const;

  /**
   * Detect executable type from file extension
   */
  static ExecutableType detect_executable_type(const std::string& file_path);

  /**
   * Convert ExecutableType to string
   */
  static std::string executable_type_to_string(ExecutableType type);

private:
  // Hot Reload Implementation Details
  struct FileWatcherImpl;
  std::unique_ptr<FileWatcherImpl> watcher_impl_;

  struct VersionManagerImpl;
  std::unique_ptr<VersionManagerImpl> version_manager_impl_;

  HotReloadConfig config_;
  HotReloadStatus status_;
  std::vector<FileChangeCallback> file_change_callbacks_;

  // Internal hot reload methods
  void        on_file_changed(const FileChange& change);
  std::string calculate_checksum(const std::string& file_path) const;
  bool        validate_file(const std::string& file_path) const;
  bool        create_backup(const std::string& file_path, const std::string& backup_path) const;

  // SO library handle (platform-specific)
  struct SoLibraryHandle;
  struct SoLibraryHandleDeleter {
    void operator()(SoLibraryHandle* handle);
  };

  using SoLibraryPtr = std::unique_ptr<SoLibraryHandle, SoLibraryHandleDeleter>;

  // Cache loaded SO libraries
  struct CachedSoLibrary {
    SoLibraryPtr                          handle;
    std::string                           version_id;
    std::chrono::system_clock::time_point load_time;
    std::string                           file_path;
  };

  std::map<std::string, CachedSoLibrary> so_library_cache_;
  mutable std::mutex                     so_cache_mutex_;

  // Helper methods
  ExecutionResult execute_command(const std::string& command, const std::string& working_dir,
                                  int timeout_ms, const std::vector<std::string>& env_vars);

  SoLibraryPtr load_so_library(const std::string& file_path);
  SoFunction   get_so_function(const SoLibraryPtr& handle, const std::string& function_name);
  bool         unload_so_library(const std::string& file_path);

  std::string build_command_string(const std::string&              executable,
                                   const std::vector<std::string>& args) const;

  void log_execution_start(const std::string& file_path, ExecutableType type);
  void log_execution_result(const std::string& file_path, const ExecutionResult& result);
};

/**
 * Get the global code loader instance
 */
CodeLoader& global_code_loader();

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_CODE_LOADER_H
