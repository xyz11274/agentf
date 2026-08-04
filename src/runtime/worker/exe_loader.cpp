#include "runtime/exe_loader.h"

#include "agent/data_variant_utils.h"
#include "agentflow/so_node.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/wait.h>

#include "utils/logger.h"

namespace agentflow {
namespace runtime {

namespace fs = std::filesystem;

// ============================================================================
// FileWatcherImpl - Platform-specific file watching implementation
// ============================================================================

struct CodeLoader::FileWatcherImpl {
  std::map<std::string, std::chrono::system_clock::time_point> last_modified_times_;
  std::thread                                                  watcher_thread_;
  std::atomic<bool>                                            running_{ false };
  mutable std::mutex                                           mutex_;
  CodeLoader*                                                  loader_;

  FileWatcherImpl(CodeLoader* ldr) : loader_(ldr) {}

  ~FileWatcherImpl() { stop(); }

  void start(const HotReloadConfig& config) {
    running_        = true;
    watcher_thread_ = std::thread(&FileWatcherImpl::watch_loop, this, config);
    Logger::instance().info("[FILE_WATCHER] File watcher started");
  }

  void stop() {
    running_ = false;
    if(watcher_thread_.joinable()) {
      watcher_thread_.join();
    }
    Logger::instance().info("[FILE_WATCHER] File watcher stopped");
  }

  void watch_loop(const HotReloadConfig& config) {
    while(running_) {
      check_files(config);
      std::this_thread::sleep_for(std::chrono::milliseconds(config.check_interval_ms));
    }
  }

  void check_files(const HotReloadConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    for(auto& pair : last_modified_times_) {
      const std::string& file_path     = pair.first;
      auto&              last_modified = pair.second;

      std::error_code ec;
      auto            fs_time = fs::last_write_time(file_path, ec);

      // Convert filesystem time to system time
      auto current_time = std::chrono::system_clock::now();
      auto fs_time_sys  = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          fs_time - std::filesystem::file_time_type::clock::now() + current_time);

      if(!ec && fs_time_sys > last_modified) {
        last_modified = fs_time_sys;

        FileChange change;
        change.file_path  = file_path;
        change.event_type = FileChangeEvent::MODIFIED;
        change.timestamp  = current_time;

        Logger::instance().info_f("[FILE_WATCHER] File changed: %s", file_path.c_str());

        loader_->on_file_changed(change);

        // Auto reload if enabled
        if(config.auto_reload && config.enabled) {
          Logger::instance().info_f("[HOT_RELOAD] Auto reloading: %s", file_path.c_str());
          auto result = loader_->reload_code(file_path);
          if(result.success) {
            Logger::instance().info_f("[HOT_RELOAD] Reload success: %s, Version=%s",
                                      file_path.c_str(), result.version_id.c_str());
          } else {
            Logger::instance().error_f("[HOT_RELOAD] Reload failed: %s, Error=%s",
                                       file_path.c_str(), result.message.c_str());
          }
        }
      }
    }
  }

  void add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::error_code ec;
    auto            fs_time = fs::last_write_time(file_path, ec);
    if(!ec) {
      auto current_time = std::chrono::system_clock::now();
      auto sys_time     = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          fs_time - std::filesystem::file_time_type::clock::now() + current_time);
      last_modified_times_[file_path] = sys_time;
      Logger::instance().info_f("[FILE_WATCHER] Started watching: %s", file_path.c_str());
    }
  }

  void remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_modified_times_.erase(file_path);
    Logger::instance().info_f("[FILE_WATCHER] Stopped watching: %s", file_path.c_str());
  }

  bool is_watching(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_modified_times_.find(file_path) != last_modified_times_.end();
  }

  size_t get_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_modified_times_.size();
  }
};

// ============================================================================
// VersionManagerImpl - Version management implementation
// ============================================================================

struct CodeLoader::VersionManagerImpl {
  struct VersionHistory {
    std::vector<std::shared_ptr<ExecutableVersion>> versions;
    size_t                                          max_versions;

    VersionHistory(size_t max_vers = 5) : max_versions(max_vers) {}

    void add_version(std::shared_ptr<ExecutableVersion> version) {
      // Mark all existing versions as inactive
      for(auto& v : versions) {
        v->is_active = false;
      }

      // Add new version as active
      version->is_active = true;
      versions.push_back(version);

      // Keep only max_versions
      while(versions.size() > max_versions) {
        versions.erase(versions.begin());
      }
    }

    std::shared_ptr<ExecutableVersion> get_active() const {
      for(auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if((*it)->is_active) {
          return *it;
        }
      }
      return nullptr;
    }

    std::shared_ptr<ExecutableVersion> find_version(const std::string& version_id) const {
      for(const auto& v : versions) {
        if(v->version_id == version_id) {
          return v;
        }
      }
      return nullptr;
    }
  };

  std::map<std::string, VersionHistory> file_versions_;
  mutable std::mutex                    mutex_;
  std::atomic<uint64_t>                 version_counter_{ 0 };

  void add_version(const std::string& file_path, std::shared_ptr<ExecutableVersion> version,
                   size_t max_versions) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& history        = file_versions_[file_path];
    history.max_versions = max_versions;
    history.add_version(version);
  }

  std::shared_ptr<ExecutableVersion> get_current_version(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_versions_.find(file_path);
    if(it != file_versions_.end()) {
      return it->second.get_active();
    }
    return nullptr;
  }

  std::vector<std::shared_ptr<ExecutableVersion>> get_all_versions(
      const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_versions_.find(file_path);
    if(it != file_versions_.end()) {
      return it->second.versions;
    }
    return {};
  }

  bool rollback_to_version(const std::string& file_path, const std::string& version_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_versions_.find(file_path);
    if(it == file_versions_.end()) {
      return false;
    }

    auto& history = it->second;
    auto  target  = history.find_version(version_id);
    if(!target) {
      return false;
    }

    // Mark all versions as inactive
    for(auto& v : history.versions) {
      v->is_active = false;
    }

    // Mark target as active
    target->is_active = true;

    Logger::instance().info_f("[VERSION_MANAGER] Rolled back %s to version %s", file_path.c_str(),
                              version_id.c_str());
    return true;
  }

  std::string generate_version_id() { return "v" + std::to_string(++version_counter_); }
};

namespace {

bool is_executable_path(const std::string& path) {
  struct stat st;
  if(stat(path.c_str(), &st) != 0) {
    return false;
  }
  if(S_ISDIR(st.st_mode)) {
    return false;
  }
  return access(path.c_str(), X_OK) == 0;
}

std::string resolve_python_command(const char* python_path) {
  if(!python_path || *python_path == '\0') {
    return "";
  }

  std::string candidate = python_path;
  if(is_executable_path(candidate)) {
    return candidate;
  }

  struct stat st;
  if(stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    std::string from_dir = candidate + "/bin/python3";
    if(is_executable_path(from_dir)) {
      return from_dir;
    }
  }

  return "";
}

std::string resolve_go_command(const char* go_path) {
  if(!go_path || *go_path == '\0') {
    return "";
  }

  std::string candidate = go_path;
  if(is_executable_path(candidate)) {
    return candidate;
  }

  struct stat st;
  if(stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    std::string from_dir = candidate + "/bin/go";
    if(is_executable_path(from_dir)) {
      return from_dir;
    }
  }

  return "";
}

}  // namespace

// ============================================================================
// SoLibraryHandle - Platform-specific SO library handle
// ============================================================================

struct CodeLoader::SoLibraryHandle {
  void* handle;

  explicit SoLibraryHandle(void* h) : handle(h) {}

  ~SoLibraryHandle() {
    if(handle) {
      dlclose(handle);
      Logger::instance().debug("[CODE_LOADER] SO library closed");
    }
  }
};

void CodeLoader::SoLibraryHandleDeleter::operator()(SoLibraryHandle* handle) {
  delete handle;
}

// ============================================================================
// CodeLoader implementation
// ============================================================================

CodeLoader::CodeLoader()
    : status_(HotReloadStatus::IDLE),
      watcher_impl_(std::make_unique<FileWatcherImpl>(this)),
      version_manager_impl_(std::make_unique<VersionManagerImpl>()) {
  Logger::instance().info("[CODE_LOADER] CodeLoader initialized");
}

CodeLoader::~CodeLoader() {
  if(status_ == HotReloadStatus::WATCHING) {
    watcher_impl_->stop();
  }

  // Unload all cached SO libraries
  std::lock_guard<std::mutex> lock(so_cache_mutex_);

  for(auto& pair : so_library_cache_) {
    Logger::instance().info_f("[CODE_LOADER] Unloading: %s", pair.second.file_path.c_str());
  }

  so_library_cache_.clear();
  Logger::instance().info("[CODE_LOADER] CodeLoader destroyed");
}

std::string CodeLoader::executable_type_to_string(ExecutableType type) {
  switch(type) {
    case ExecutableType::SO_LIBRARY:
      return "SO_LIBRARY";
    case ExecutableType::JAR_FILE:
      return "JAR_FILE";
    case ExecutableType::SHELL_SCRIPT:
      return "SHELL_SCRIPT";
    case ExecutableType::GO_SCRIPT:
      return "GO_SCRIPT";
    case ExecutableType::PYTHON_SCRIPT:
      return "PYTHON_SCRIPT";
    case ExecutableType::EXECUTABLE:
      return "EXECUTABLE";
    default:
      return "UNKNOWN";
  }
}

ExecutableType CodeLoader::detect_executable_type(const std::string& file_path) {
  std::string ext = fs::path(file_path).extension().string();

  // Convert to lowercase for case-insensitive comparison
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if(ext == ".so" || ext == ".dylib" || ext == ".dll") {
    return ExecutableType::SO_LIBRARY;
  } else if(ext == ".jar") {
    return ExecutableType::JAR_FILE;
  } else if(ext == ".sh" || ext == ".bash") {
    return ExecutableType::SHELL_SCRIPT;
  } else if(ext == ".go") {
    return ExecutableType::GO_SCRIPT;
  } else if(ext == ".py") {
    return ExecutableType::PYTHON_SCRIPT;
  } else if(ext.empty() || ext == ".exe" || ext == ".bin") {
    // Check if file has execute permission
    std::error_code ec;
    auto            perms = fs::status(file_path, ec).permissions();
    if(!ec && (perms & fs::perms::owner_exec) != fs::perms::none) {
      return ExecutableType::EXECUTABLE;
    }
  }

  return ExecutableType::UNKNOWN;
}

ExecutionResult CodeLoader::load_and_execute_so(const std::string&     file_path,
                                                const std::string&     function_name,
                                                const ExecutionConfig& config) {
  // If databus_client is provided, delegate to V3 version
  if(config.databus_client) {
    return load_and_execute_so_v3(file_path, function_name, config, config.databus_client);
  }
  // If databus is provided, delegate to V2 version
  if(config.databus) {
    return load_and_execute_so_v2(file_path, function_name, config, config.databus);
  }

  auto start_time = std::chrono::steady_clock::now();

  log_execution_start(file_path, ExecutableType::SO_LIBRARY);

  ExecutionResult result;

  // Check if hot reload is enabled and we need to reload
  bool needs_reload = false;
  if(is_watching(file_path)) {
    auto current_version = get_current_version(file_path);
    auto latest_version  = get_current_version(file_path);

    if(!current_version || !latest_version
       || current_version->version_id != latest_version->version_id) {
      needs_reload = true;
    }
  }

  // Load or reload the SO library
  SoLibraryPtr so_handle;
  SoFunction   func = nullptr;

  {
    std::lock_guard<std::mutex> lock(so_cache_mutex_);

    if(needs_reload) {
      unload_so_library(file_path);
    }

    auto it = so_library_cache_.find(file_path);
    if(it != so_library_cache_.end()) {
      // Get function pointer from cached handle
      func = get_so_function(it->second.handle, function_name);
      if(!func) {
        result.success       = false;
        result.error_message = "Function not found: " + function_name;
        result.exit_code     = -1;
        return result;
      }
      Logger::instance().info_f("[CODE_LOADER] Using cached SO library: %s", file_path.c_str());
    } else {
      auto loaded_handle = load_so_library(file_path);
      if(!loaded_handle) {
        result.success       = false;
        result.error_message = "Failed to load SO library";
        result.exit_code     = -1;
        return result;
      }

      // Get function pointer before caching
      func = get_so_function(loaded_handle, function_name);
      if(!func) {
        result.success       = false;
        result.error_message = "Function not found: " + function_name;
        result.exit_code     = -1;
        return result;
      }

      // Cache the library
      CachedSoLibrary cached;
      cached.handle    = std::move(loaded_handle);
      cached.load_time = std::chrono::system_clock::now();
      cached.file_path = file_path;

      auto version = get_current_version(file_path);
      if(version) {
        cached.version_id = version->version_id;
      }

      so_library_cache_[file_path] = std::move(cached);
    }
  }

  // Get function pointer (now we have it from above)
  if(!func) {
    result.success       = false;
    result.error_message = "Function not found: " + function_name;
    result.exit_code     = -1;
    return result;
  }

  // Execute the function
  try {
    DataMap outputs;
    func(config.inputs, outputs);

    // Convert outputs to string
    for(const auto& pair : outputs) {
      std::string value_str = data_variant_to_string(pair.second);

      result.stdout += pair.first + "=" + value_str + "\n";
    }

    result.success   = true;
    result.exit_code = 0;
  } catch(const std::exception& e) {
    result.success       = false;
    result.error_message = "SO function exception: " + std::string(e.what());
    result.exit_code     = -1;
  } catch(...) {
    result.success       = false;
    result.error_message = "SO function: unknown exception";
    result.exit_code     = -1;
  }

  auto end_time = std::chrono::steady_clock::now();
  result.duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  log_execution_result(file_path, result);

  return result;
}

ExecutionResult CodeLoader::load_and_execute_so_v2(const std::string&     file_path,
                                                   const std::string&     function_name,
                                                   const ExecutionConfig& config,
                                                   INodeDataBus*          databus) {
  auto start_time = std::chrono::steady_clock::now();

  log_execution_start(file_path, ExecutableType::SO_LIBRARY);

  ExecutionResult result;

  // Check if hot reload is enabled and we need to reload
  bool needs_reload = false;
  if(is_watching(file_path)) {
    auto current_version = get_current_version(file_path);
    auto latest_version  = get_current_version(file_path);

    if(!current_version || !latest_version
       || current_version->version_id != latest_version->version_id) {
      needs_reload = true;
    }
  }

  // Load or reload the SO library
  SoLibraryPtr   so_handle;
  SoFunctionV2   func_v2 = nullptr;
  SoFunction     func_v1 = nullptr;

  {
    std::lock_guard<std::mutex> lock(so_cache_mutex_);

    if(needs_reload) {
      unload_so_library(file_path);
    }

    auto it = so_library_cache_.find(file_path);
    if(it != so_library_cache_.end()) {
      // Try V2 signature first
      func_v2 = get_so_function_v2(it->second.handle, function_name);
      if(!func_v2) {
        // Fall back to V1 signature
        func_v1 = get_so_function(it->second.handle, function_name);
      }
      if(!func_v2 && !func_v1) {
        result.success       = false;
        result.error_message = "Function not found: " + function_name;
        result.exit_code     = -1;
        return result;
      }
      Logger::instance().info_f("[CODE_LOADER] Using cached SO library: %s (V2=%s)",
                                file_path.c_str(), func_v2 ? "yes" : "no");
    } else {
      auto loaded_handle = load_so_library(file_path);
      if(!loaded_handle) {
        result.success       = false;
        result.error_message = "Failed to load SO library";
        result.exit_code     = -1;
        return result;
      }

      // Try V2 signature first
      func_v2 = get_so_function_v2(loaded_handle, function_name);
      if(!func_v2) {
        // Fall back to V1 signature
        func_v1 = get_so_function(loaded_handle, function_name);
      }
      if(!func_v2 && !func_v1) {
        result.success       = false;
        result.error_message = "Function not found: " + function_name;
        result.exit_code     = -1;
        return result;
      }

      // Cache the library
      CachedSoLibrary cached;
      cached.handle    = std::move(loaded_handle);
      cached.load_time = std::chrono::system_clock::now();
      cached.file_path = file_path;

      auto version = get_current_version(file_path);
      if(version) {
        cached.version_id = version->version_id;
      }

      so_library_cache_[file_path] = std::move(cached);
    }
  }

  // Execute the function (prefer V2 if available)
  try {
    DataMap outputs;

    if(func_v2) {
      // V2 signature with DataBus
      func_v2(config.inputs, outputs, databus);
    } else if(func_v1) {
      // V1 signature without DataBus
      func_v1(config.inputs, outputs);
    }

    // Convert outputs to string
    for(const auto& pair : outputs) {
      std::string value_str = data_variant_to_string(pair.second);

      result.stdout += pair.first + "=" + value_str + "\n";
    }

    result.success   = true;
    result.exit_code = 0;
  } catch(const std::exception& e) {
    result.success       = false;
    result.error_message = "SO function exception: " + std::string(e.what());
    result.exit_code     = -1;
  } catch(...) {
    result.success       = false;
    result.error_message = "SO function: unknown exception";
    result.exit_code     = -1;
  }

  auto end_time = std::chrono::steady_clock::now();
  result.duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  log_execution_result(file_path, result);

  return result;
}

ExecutionResult CodeLoader::load_and_execute_so_v3(const std::string&     file_path,
                                                   const std::string&     function_name,
                                                   const ExecutionConfig& config,
                                                   INodeDataBusClient*    client) {
  auto start_time = std::chrono::steady_clock::now();

  log_execution_start(file_path, ExecutableType::SO_LIBRARY);

  ExecutionResult result;

  // Check if hot reload is enabled and we need to reload
  bool needs_reload = false;
  if(is_watching(file_path)) {
    auto current_version = get_current_version(file_path);
    auto latest_version  = get_current_version(file_path);

    if(!current_version || !latest_version
       || current_version->version_id != latest_version->version_id) {
      needs_reload = true;
    }
  }

  // Load or reload the SO library
  SoLibraryPtr   so_handle;
  SoFunctionV3   func_v3 = nullptr;
  SoFunctionV2   func_v2 = nullptr;
  SoFunction     func_v1 = nullptr;

  {
    std::lock_guard<std::mutex> lock(so_cache_mutex_);

    if(needs_reload) {
      unload_so_library(file_path);
    }

    auto it = so_library_cache_.find(file_path);
    if(it != so_library_cache_.end()) {
      // Try V3 signature first, then V2, then V1
      func_v3 = get_so_function_v3(it->second.handle, function_name);
      if(!func_v3) {
        func_v2 = get_so_function_v2(it->second.handle, function_name);
      }
      if(!func_v3 && !func_v2) {
        func_v1 = get_so_function(it->second.handle, function_name);
      }
      if(!func_v3 && !func_v2 && !func_v1) {
        result.success       = false;
        result.error_message = "Function not found: " + function_name;
        result.exit_code     = -1;
        return result;
      }
      Logger::instance().info_f("[CODE_LOADER] Using cached SO library: %s (V3=%s, V2=%s)",
                                file_path.c_str(), func_v3 ? "yes" : "no", func_v2 ? "yes" : "no");
    } else {
      auto loaded_handle = load_so_library(file_path);
      if(!loaded_handle) {
        result.success       = false;
        result.error_message = "Failed to load SO library";
        result.exit_code     = -1;
        return result;
      }

      // Try V3 signature first, then V2, then V1
      func_v3 = get_so_function_v3(loaded_handle, function_name);
      if(!func_v3) {
        func_v2 = get_so_function_v2(loaded_handle, function_name);
      }
      if(!func_v3 && !func_v2) {
        func_v1 = get_so_function(loaded_handle, function_name);
      }
      if(!func_v3 && !func_v2 && !func_v1) {
        result.success       = false;
        result.error_message = "Function not found: " + function_name;
        result.exit_code     = -1;
        return result;
      }

      // Cache the library
      CachedSoLibrary cached;
      cached.handle    = std::move(loaded_handle);
      cached.load_time = std::chrono::system_clock::now();
      cached.file_path = file_path;

      auto version = get_current_version(file_path);
      if(version) {
        cached.version_id = version->version_id;
      }

      so_library_cache_[file_path] = std::move(cached);
    }
  }

  // Execute the function (prefer V3, then V2, then V1)
  try {
    DataMap outputs;

    if(func_v3 && client) {
      // V3 signature with session-isolated DataBus client
      func_v3(config.inputs, outputs, client);
    } else if(func_v2 && config.databus) {
      // V2 signature with DataBus
      func_v2(config.inputs, outputs, config.databus);
    } else if(func_v1) {
      // V1 signature without DataBus
      func_v1(config.inputs, outputs);
    } else {
      result.success       = false;
      result.error_message = "No compatible function signature found or missing DataBus client";
      result.exit_code     = -1;
      return result;
    }

    // Convert outputs to string
    for(const auto& pair : outputs) {
      std::string value_str = data_variant_to_string(pair.second);

      result.stdout += pair.first + "=" + value_str + "\n";
    }

    result.success   = true;
    result.exit_code = 0;
  } catch(const std::exception& e) {
    result.success       = false;
    result.error_message = "SO function exception: " + std::string(e.what());
    result.exit_code     = -1;
  } catch(...) {
    result.success       = false;
    result.error_message = "SO function: unknown exception";
    result.exit_code     = -1;
  }

  auto end_time = std::chrono::steady_clock::now();
  result.duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  log_execution_result(file_path, result);

  return result;
}

ExecutionResult CodeLoader::execute_jar(const std::string& jar_path, const std::string& main_class,
                                        const std::vector<std::string>& args,
                                        const ExecutionConfig&          config,
                                        const sandbox::SandboxOptions*  sandbox_opts) {
  log_execution_start(jar_path, ExecutableType::JAR_FILE);

  // Check if java is available
  std::string java_cmd  = "java";
  const char* java_path = std::getenv("JAVA_HOME");
  if(java_path) {
    java_cmd = std::string(java_path) + "/bin/java";
  }

  // Build command: java -jar <jar_path> [args]
  std::vector<std::string> cmd_args = { "-jar", jar_path };
  if(!main_class.empty()) {
    cmd_args = { "-cp", jar_path, main_class };
  }
  cmd_args.insert(cmd_args.end(), args.begin(), args.end());

  std::string command = build_command_string(java_cmd, cmd_args);
  return execute_command(command, config.working_dir, config.timeout_ms, config.env_vars, sandbox_opts);
}

ExecutionResult CodeLoader::execute_shell_script(const std::string&              script_path,
                                                 const std::vector<std::string>& args,
                                                 const ExecutionConfig&          config,
                                                 const sandbox::SandboxOptions*  sandbox_opts) {
  log_execution_start(script_path, ExecutableType::SHELL_SCRIPT);

  std::string command = build_command_string(script_path, args);
  return execute_command(command, config.working_dir, config.timeout_ms, config.env_vars, sandbox_opts);
}

ExecutionResult CodeLoader::execute_python_script(const std::string&              script_path,
                                                  const std::vector<std::string>& args,
                                                  const ExecutionConfig&          config,
                                                  const sandbox::SandboxOptions*  sandbox_opts) {
  log_execution_start(script_path, ExecutableType::PYTHON_SCRIPT);

  std::string python_cmd  = "python3";
  const char* python_path = std::getenv("PYTHON_PATH");
  std::string resolved_path = resolve_python_command(python_path);
  if(!resolved_path.empty()) {
    python_cmd = resolved_path;
  }

  std::string command = build_command_string(python_cmd, { script_path });
  command += " " + build_command_string("", args);

  return execute_command(command, config.working_dir, config.timeout_ms, config.env_vars, sandbox_opts);
}

ExecutionResult CodeLoader::execute_go_script(const std::string&              script_path,
                                              const std::vector<std::string>& args,
                                              const ExecutionConfig&          config,
                                              const sandbox::SandboxOptions*  sandbox_opts) {
  log_execution_start(script_path, ExecutableType::GO_SCRIPT);

  std::string go_cmd  = "go";
  const char* go_path = std::getenv("GO_PATH");
  std::string resolved_path = resolve_go_command(go_path);
  if(!resolved_path.empty()) {
    go_cmd = resolved_path;
  }

  std::string command = build_command_string(go_cmd, { "run", script_path });
  command += " " + build_command_string("", args);

  return execute_command(command, config.working_dir, config.timeout_ms, config.env_vars, sandbox_opts);
}

ExecutionResult CodeLoader::execute(const std::string& file_path, const ExecutionConfig& config,
                                    const sandbox::SandboxOptions* sandbox_opts) {
  ExecutableType type = detect_executable_type(file_path);

  switch(type) {
    case ExecutableType::SO_LIBRARY:
      return load_and_execute_so(file_path, "execute", config);

    case ExecutableType::JAR_FILE:
      return execute_jar(file_path, "", {}, config, sandbox_opts);

    case ExecutableType::SHELL_SCRIPT:
      return execute_shell_script(file_path, {}, config, sandbox_opts);

    case ExecutableType::GO_SCRIPT:
      return execute_go_script(file_path, {}, config, sandbox_opts);

    case ExecutableType::PYTHON_SCRIPT:
      return execute_python_script(file_path, {}, config, sandbox_opts);

    case ExecutableType::EXECUTABLE:
      return execute_shell_script(file_path, {}, config, sandbox_opts);

    default:
      ExecutionResult result;
      result.success       = false;
      result.error_message = "Unknown executable type";
      return result;
  }
}



// ============================================================================
// Hot Reload Methods
// ============================================================================

void CodeLoader::set_hot_reload_config(const HotReloadConfig& config) {
  config_ = config;

  if(config_.enabled && status_ == HotReloadStatus::IDLE) {
    status_ = HotReloadStatus::WATCHING;
    watcher_impl_->start(config_);
    Logger::instance().info("[HOT_RELOAD] Hot reload enabled");
  } else if(!config_.enabled && status_ == HotReloadStatus::WATCHING) {
    watcher_impl_->stop();
    status_ = HotReloadStatus::IDLE;
    Logger::instance().info("[HOT_RELOAD] Hot reload disabled");
  }
}

const HotReloadConfig& CodeLoader::get_hot_reload_config() const {
  return config_;
}

bool CodeLoader::start_watching(const std::string& file_path) {
  if(!fs::exists(file_path)) {
    Logger::instance().error_f("[HOT_RELOAD] File not found: %s", file_path.c_str());
    return false;
  }

  watcher_impl_->add_file(file_path);

  // Create initial version
  auto version        = std::make_shared<ExecutableVersion>();
  version->version_id = version_manager_impl_->generate_version_id();
  version->file_path  = file_path;
  version->checksum   = calculate_checksum(file_path);

  std::error_code ec;
  version->file_size     = fs::file_size(file_path, ec);
  auto fs_time           = fs::last_write_time(file_path, ec);
  auto current_time      = std::chrono::system_clock::now();
  version->modified_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      fs_time - std::filesystem::file_time_type::clock::now() + current_time);

  version_manager_impl_->add_version(file_path, version, config_.max_versions);

  Logger::instance().info_f("[HOT_RELOAD] Started watching: %s (Version=%s)", file_path.c_str(),
                            version->version_id.c_str());

  return true;
}

bool CodeLoader::stop_watching(const std::string& file_path) {
  watcher_impl_->remove_file(file_path);
  return true;
}

bool CodeLoader::is_watching(const std::string& file_path) const {
  return watcher_impl_->is_watching(file_path);
}

void CodeLoader::register_file_change_callback(FileChangeCallback callback) {
  file_change_callbacks_.push_back(callback);
}

std::shared_ptr<ExecutableVersion> CodeLoader::get_current_version(
    const std::string& file_path) const {
  return version_manager_impl_->get_current_version(file_path);
}

std::vector<std::shared_ptr<ExecutableVersion>> CodeLoader::get_all_versions(
    const std::string& file_path) const {
  return version_manager_impl_->get_all_versions(file_path);
}

bool CodeLoader::rollback_to_version(const std::string& file_path, const std::string& version_id) {
  return version_manager_impl_->rollback_to_version(file_path, version_id);
}

ReloadResult CodeLoader::reload_code(const std::string& file_path) {
  auto start_time = std::chrono::steady_clock::now();

  // Resolve actual file path: try alternate extension if original doesn't exist
  std::string actual_path = file_path;
  if(!fs::exists(actual_path)) {
    if(file_path.size() > 3 && file_path.substr(file_path.size() - 3) == ".so") {
      std::string alt = file_path.substr(0, file_path.size() - 3) + ".dylib";
      if(fs::exists(alt)) { actual_path = alt; }
    } else if(file_path.size() > 6 && file_path.substr(file_path.size() - 6) == ".dylib") {
      std::string alt = file_path.substr(0, file_path.size() - 6) + ".so";
      if(fs::exists(alt)) { actual_path = alt; }
    }
  }

  Logger::instance().info_f("[HOT_RELOAD] Reloading: %s", actual_path.c_str());

  ReloadResult result;

  // Validate file if required
  if(config_.validate_before_reload) {
    if(!validate_file(actual_path)) {
      result.success = false;
      result.message = "File validation failed";
      Logger::instance().error_f("[HOT_RELOAD] Validation failed: %s", actual_path.c_str());
      return result;
    }
  }

  // Create backup if required
  if(config_.backup_before_reload) {
    std::string backup_path =
        config_.backup_dir.empty()
            ? actual_path + ".bak"
            : config_.backup_dir + "/" + fs::path(actual_path).filename().string() + ".bak";

    if(!create_backup(actual_path, backup_path)) {
      Logger::instance().warn_f("[HOT_RELOAD] Failed to create backup: %s", backup_path.c_str());
    }
  }

  // Create new version
  auto version        = std::make_shared<ExecutableVersion>();
  version->version_id = version_manager_impl_->generate_version_id();
  version->file_path  = file_path;
  version->checksum   = calculate_checksum(file_path);

  std::error_code ec;
  version->file_size     = fs::file_size(file_path, ec);
  auto fs_time           = fs::last_write_time(file_path, ec);
  auto current_time      = std::chrono::system_clock::now();
  version->modified_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      fs_time - std::filesystem::file_time_type::clock::now() + current_time);

  version_manager_impl_->add_version(file_path, version, config_.max_versions);

  result.success    = true;
  result.version_id = version->version_id;
  result.message    = "Reload successful";

  auto end_time   = std::chrono::steady_clock::now();
  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  Logger::instance().info_f("[HOT_RELOAD] Reload complete: %s, Version=%s, Duration=%ldms",
                            file_path.c_str(), version->version_id.c_str(),
                            result.duration.count());

  return result;
}

void CodeLoader::reload_code_async(const std::string& file_path, ReloadCallback callback) {
  std::thread([this, file_path, callback]() {
    auto result = reload_code(file_path);
    if(callback) {
      callback(result);
    }
  }).detach();
}

HotReloadStatus CodeLoader::get_hot_reload_status() const {
  return status_;
}

size_t CodeLoader::get_watched_files_count() const {
  return watcher_impl_->get_count();
}

void CodeLoader::on_file_changed(const FileChange& change) {
  for(const auto& callback : file_change_callbacks_) {
    try {
      callback(change);
    } catch(const std::exception& e) {
      Logger::instance().error_f("[HOT_RELOAD] Callback exception: %s", e.what());
    } catch(...) {
      Logger::instance().error("[HOT_RELOAD] Callback: unknown exception");
    }
  }
}

std::string CodeLoader::calculate_checksum(const std::string& file_path) const {
  // Simple checksum implementation (can be replaced with MD5/SHA256)
  std::ifstream file(file_path, std::ios::binary);
  if(!file) {
    return "";
  }

  uint32_t checksum = 0;
  char     buffer[4096];

  while(file.read(buffer, sizeof(buffer))) {
    for(size_t i = 0; i < file.gcount(); ++i) {
      checksum = (checksum << 8) | (checksum >> 24);
      checksum += static_cast<uint8_t>(buffer[i]);
    }
  }

  // Handle remaining bytes
  for(size_t i = 0; i < file.gcount(); ++i) {
    checksum = (checksum << 8) | (checksum >> 24);
    checksum += static_cast<uint8_t>(buffer[i]);
  }

  char checksum_str[16];
  snprintf(checksum_str, sizeof(checksum_str), "%08x", checksum);
  return std::string(checksum_str);
}

bool CodeLoader::validate_file(const std::string& file_path) const {
  // Basic file validation
  std::error_code ec;
  if(!fs::exists(file_path, ec)) {
    return false;
  }

  auto file_size = fs::file_size(file_path, ec);
  if(ec || file_size == 0) {
    return false;
  }

  // Check if file is readable
  std::ifstream file(file_path, std::ios::binary);
  if(!file) {
    return false;
  }

  return true;
}

bool CodeLoader::create_backup(const std::string& file_path, const std::string& backup_path) const {
  try {
    // Create backup directory if it doesn't exist
    fs::create_directories(fs::path(backup_path).parent_path());

    // Copy file
    fs::copy_file(file_path, backup_path, fs::copy_options::overwrite_existing);

    Logger::instance().info_f("[HOT_RELOAD] Backup created: %s -> %s", file_path.c_str(),
                              backup_path.c_str());
    return true;
  } catch(const std::exception& e) {
    Logger::instance().error_f("[HOT_RELOAD] Backup failed: %s", e.what());
    return false;
  }
}


ExecutionResult CodeLoader::execute_command(const std::string& command,
                                            const std::string& working_dir, int timeout_ms,
                                            const std::vector<std::string>& env_vars,
                                            const sandbox::SandboxOptions* sandbox_opts) {
  ExecutionResult result;
  auto            start_time = std::chrono::steady_clock::now();

  Logger::instance().debug_f("[CODE_LOADER] Executing command: %s (timeout_ms=%d)", command.c_str(),
                             timeout_ms);

  // [DEBUG] Log full command details for troubleshooting
  Logger::instance().debug_f("[CODE_LOADER_DEBUG] Command=%s, WorkingDir=%s, EnvVars=%zu",
                            command.c_str(), working_dir.c_str(), env_vars.size());

  auto shell_escape_single = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(char ch : s) {
      if(ch == '\'') {
        out += "'\\''";
      } else {
        out += ch;
      }
    }
    return out;
  };
  std::string prepared_command = command;
  if(!working_dir.empty()) {
    // Resolve to an absolute path IMMEDIATELY before use, because the
    // daemon's process-level CWD (getcwd) is NOT thread-safe — another
    // thread may call chdir() concurrently, making a subsequent `cd <rel>`
    // resolve to the wrong directory (observed as intermittent "exit code 2"
    // from the shell: the Python script file isn't found at the wrong CWD).
    // An absolute path is immune to CWD changes regardless of timing.
    std::string abs_wd;
    try {
      abs_wd = fs::absolute(fs::path(working_dir)).string();
    } catch(...) {
      abs_wd = working_dir;  // fallback — let cd fail naturally if invalid
    }
    prepared_command =
        "cd '" + shell_escape_single(abs_wd) + "' && (" + prepared_command + ")";
  }
  if(!env_vars.empty()) {
    std::ostringstream env_prefix;
    for(const auto& env : env_vars) {
      size_t eq = env.find('=');
      if(eq == std::string::npos || eq == 0) {
        continue;
      }
      std::string key = env.substr(0, eq);
      std::string val = env.substr(eq + 1);
      env_prefix << key << "='" << shell_escape_single(val) << "' ";
    }
    std::string exports = env_prefix.str();
    if(!exports.empty()) {
      prepared_command = "export " + exports + "; " + prepared_command;
    }
  }

  Logger::instance().debug_f("[CODE_LOADER_DEBUG] Final command: %s", prepared_command.c_str());

  int output_pipe[2];
  if(pipe(output_pipe) != 0) {
    result.success       = false;
    result.error_message = "Failed to create command output pipe";
    result.exit_code     = -1;
    return result;
  }

  // ── stderr 实时落盘 pipe（与 stdout pipe 分离）──────────────────
  // 设计不变量（见 docs/design/architecture/modules/sandbox-module.md §6.3 +
  // #1/#2 调研结论）：
  //   - result.stdout 仍只含 stdout（line ~1320 `result.stdout = output` 不变）
  //   - result.stderr 仍只在 failure 时赋（line ~1340 不变）
  //   - nullptr sandbox_opts 路径行为不变（stderr pipe 建立与否不依赖 sandbox_opts）
  //   - node 完成时机不变（poll loop 仍等 waitpid 返回）
  //   - 独立文件是**额外**的实时落盘，不替代 result.stderr
  // 动机：skill 子进程（skill_executor.py）的 emit_event/`[flow]` stderr 在 node
  // 未完成时憋在 pipe 里不落盘（poll loop 只在 waitpid 返回后 drain_output），
  // 导致 outfit-stylist skill 内部卡住 2 分 41 秒时 daemon.log 完全沈默，无法定位
  // 是哪一步慢。按 PROJECT_MEMORY pitfall（子进程场景需独立文件兠底）+ F7.1
  // （外部服务接口的原始请求/响应必须完整记录）——这里额外建一根 stderr 专用 pipe，
  // 父进程 poll loop drain 时实时追加写到独立文件，node 完成时机不变。
  int stderr_pipe[2];
  if(pipe(stderr_pipe) != 0) {
    // stderr pipe 建失败不阻断主流程——stdout pipe 仍能工作，只是失去 stderr 实时落盘
    Logger::instance().warn_f("[CODE_LOADER] Failed to create stderr drain pipe: %s", std::strerror(errno));
    stderr_pipe[0] = -1;
    stderr_pipe[1] = -1;
  }

  // 独立 stderr 日志文件路径：用 daemon 的 AF_LOG_FILE 惯例取当前 log 目录，
  // 命名 skill_stderr.<child_pid>.log。child_pid 在 fork 后才有，这里先取 daemon
  // log 目录 + 用 getpid() 标识本次 execute_command 调用（父进程 pid 唯一标识调用轮）。
  // dialogue_id/skill_name 在 execute_command 内拿不到（4 个调用点只传 command/timeout/env），
  // 用 getpid() 标识是最佳可达方案——文件内容里的 `[flow]`/`[obs]` 行自带 dialogue_id。
  std::string stderr_log_path;
  std::ofstream stderr_log_stream;
  if(stderr_pipe[0] >= 0) {
    const char* daemon_log = std::getenv("AF_LOG_FILE");
    if(daemon_log && *daemon_log) {
      // 取 daemon log 目录，建 skill_stderr.<pid>.log
      std::string dir = std::string(daemon_log);
      size_t last_sep = dir.find_last_of('/');
      if(last_sep != std::string::npos) {
        dir = dir.substr(0, last_sep);
      } else {
        dir = ".";
      }
      stderr_log_path = dir + "/skill_stderr." + std::to_string(static_cast<long>(getpid())) + ".log";
    } else {
      // 无 AF_LOG_FILE 兜底：落 /tmp/（子进程场景需独立文件兠底，避免父进程 tool_result 链路二次截断）
      stderr_log_path = "/tmp/agentflowd_skill_stderr." + std::to_string(static_cast<long>(getpid())) + ".log";
    }
    stderr_log_stream.open(stderr_log_path, std::ios::out | std::ios::app);
    if(!stderr_log_stream.is_open()) {
      Logger::instance().warn_f("[CODE_LOADER] Failed to open stderr drain log: %s", stderr_log_path.c_str());
    } else {
      stderr_log_stream << "=== execute_command start pid=" << static_cast<long>(getpid())
                        << " ts=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count()
                        << " ===\n";
      stderr_log_stream.flush();
    }
  }

  pid_t child_pid = fork();
  if(child_pid < 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    if(stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if(stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    result.success       = false;
    result.error_message = "Failed to fork command process";
    result.exit_code     = -1;
    return result;
  }

  if(child_pid == 0) {
    // A dedicated process group lets the parent terminate the shell and every
    // descendant it launches. This is essential for Python/tool subprocesses.
    setpgid(0, 0);
    dup2(output_pipe[1], STDOUT_FILENO);
    if(stderr_pipe[1] >= 0) {
      dup2(stderr_pipe[1], STDERR_FILENO);  // stderr 走专用 pipe，与 stdout 分离
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
    } else {
      dup2(output_pipe[1], STDERR_FILENO);  // fallback：stderr pipe 建失败时合并到 stdout（原行为）
    }
    close(output_pipe[0]);
    close(output_pipe[1]);

    // Do not leak daemon listeners/client sockets into node subprocesses.
    long max_fd = sysconf(_SC_OPEN_MAX);
    if(max_fd < 0) max_fd = 1024;
    // macOS commonly reports an extremely large OPEN_MAX. Walking the full
    // range delays exec long enough to race short command timeouts; descriptors
    // above this practical ceiling are not opened by the daemon.
    max_fd = std::min<long>(max_fd, 4096);
    for(int fd = 3; fd < max_fd; ++fd) close(fd);

    execl("/bin/sh", "sh", "-c", prepared_command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  // Close the child side and make reads non-blocking so timeout enforcement
  // never depends on a descendant closing the output pipe.
  close(output_pipe[1]);
  if(stderr_pipe[1] >= 0) close(stderr_pipe[1]);  // 父进程关闭子进程端，避免 fd 泄漏
  setpgid(child_pid, child_pid);
  int flags = fcntl(output_pipe[0], F_GETFL, 0);
  if(flags >= 0) fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
  if(stderr_pipe[0] >= 0) {
    int sflags = fcntl(stderr_pipe[0], F_GETFL, 0);
    if(sflags >= 0) fcntl(stderr_pipe[0], F_SETFL, sflags | O_NONBLOCK);
  }

  char buffer[4096];
  std::string output;
  std::string stderr_buffer;  // 累积 stderr（可选赋给 result.stderr，但默认不赋——独立文件是主落盘）
  int status = 0;
  bool child_running = true;
  bool timed_out = false;

  auto drain_output = [&]() {
    while(true) {
      ssize_t n = read(output_pipe[0], buffer, sizeof(buffer));
      if(n > 0) {
        output.append(buffer, static_cast<size_t>(n));
      } else if(n < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
  };

  // stderr 实时落盘 lambda——每次 drain 把 stderr pipe 内容追加写到独立文件。
  // 不阻断主流程：read EAGAIN 时静默返回（非阻塞 pipe）；写文件失败只 WARN 不抛。
  auto drain_stderr_to_log = [&]() {
    if(stderr_pipe[0] < 0 || !stderr_log_stream.is_open()) return;
    while(true) {
      ssize_t n = read(stderr_pipe[0], buffer, sizeof(buffer));
      if(n > 0) {
        stderr_buffer.append(buffer, static_cast<size_t>(n));
        stderr_log_stream.write(buffer, static_cast<std::streamsize>(n));
        stderr_log_stream.flush();  // 实时落盘——子进程卡住时立即可见
      } else if(n < 0 && errno == EINTR) {
        continue;
      } else {
        break;  // EAGAIN (非阻塞 pipe 无数据) 或 EOF
      }
    }
  };

  while(child_running) {
    drain_output();
    drain_stderr_to_log();  // 每次 poll 轮 drain stderr 实时落盘

    pid_t waited = waitpid(child_pid, &status, WNOHANG);
    if(waited == child_pid) {
      child_running = false;
      break;
    }
    if(waited < 0 && errno != EINTR) {
      child_running = false;
      break;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    if(timeout_ms > 0 && elapsed_ms >= timeout_ms) {
      timed_out = true;
      kill(-child_pid, SIGKILL);
      Logger::instance().warn_f(
          "[NODE_TIMEOUT] child_pid=%d killed after %lldms (timeout_ms=%d); "
          "stdout buffer (%zu bytes) is residual and will not be parsed as json_output; "
          "stderr drain log (%zu bytes) at %s",
          static_cast<int>(child_pid),
          static_cast<long long>(elapsed_ms),
          timeout_ms,
          output.size(),
          stderr_buffer.size(),
          stderr_log_path.empty() ? "(disabled)" : stderr_log_path.c_str());
      while(waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {}
      child_running = false;
      break;
    }

    int wait_ms = 20;
    if(timeout_ms > 0) {
      wait_ms = static_cast<int>(std::min<int64_t>(wait_ms, timeout_ms - elapsed_ms));
      if(wait_ms < 1) wait_ms = 1;
    }
    struct pollfd pfd { output_pipe[0], POLLIN, 0 };
    poll(&pfd, 1, wait_ms);
  }

  drain_output();
  close(output_pipe[0]);
  drain_stderr_to_log();  // 子进程退出后最后一次 drain（捕获退出前的尾部 stderr）
  if(stderr_pipe[0] >= 0) close(stderr_pipe[0]);
  if(stderr_log_stream.is_open()) {
    stderr_log_stream << "=== execute_command end pid=" << static_cast<long>(getpid())
                      << " exit=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
                      << " duration_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start_time).count()
                      << " stderr_bytes=" << stderr_buffer.size() << " ===\n";
    stderr_log_stream.flush();
    stderr_log_stream.close();
  }

  // ── Stderr 回喂 daemon.log ────────────────────────────────────
  // stderr_buffer 内含子进程 emit_event / [flow] 等结构化日志，
  // 已实时落盘到独立文件 skill_stderr.<pid>.log。
  // 这里再打到 daemon.log 便于一站式检索。
  // 内容中自带 [flow] / [obs] 标记 + dialogue_id，不必额外提取。
  if (!stderr_buffer.empty()) {
    Logger::instance().info_f("[NODE_STDERR] pid=%d bytes=%zu\n%s",
                              child_pid,
                              stderr_buffer.size(),
                              stderr_buffer.c_str());
  }

  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  int signal_num = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  bool killed_by_signal = WIFSIGNALED(status);

  result.exit_code = exit_code;
  result.stdout    = output;  // Note: stdout contains both original stdout and stderr
  result.success   = (!timed_out && exit_code == 0 && !killed_by_signal);

  // [DEBUG] Log execution result details
  Logger::instance().debug_f("[CODE_LOADER_DEBUG] Exit code=%d, Success=%s, OutputLen=%zu, Signal=%d",
                            exit_code, result.success ? "true" : "false", output.size(), signal_num);
  if(!output.empty()) {
    Logger::instance().debug_f("[CODE_LOADER_DEBUG] Output: %s", output.c_str());
  }

  if(!result.success) {
    if(timed_out) {
      result.error_message = "Command execution timeout";
      result.exit_code     = -1;
    } else if(killed_by_signal) {
      result.error_message = "Command killed by signal: " + std::to_string(signal_num);
      result.exit_code     = -1;
    } else {
      result.error_message = "Command failed with exit code: " + std::to_string(exit_code);
    }
    result.stderr = output;  // stderr contains combined output for error cases
  }

  auto end_time = std::chrono::steady_clock::now();
  result.duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  return result;
}

CodeLoader::SoLibraryPtr CodeLoader::load_so_library(const std::string& file_path) {
  Logger::instance().info_f("[CODE_LOADER] Loading SO library: %s", file_path.c_str());

  void* handle = dlopen(file_path.c_str(), RTLD_LAZY);
  if(!handle) {
    // Try alternate extension: .so ↔ .dylib (cross-platform)
    std::string alt_path;
    if(file_path.size() > 3 && file_path.substr(file_path.size() - 3) == ".so") {
      alt_path = file_path.substr(0, file_path.size() - 3) + ".dylib";
    } else if(file_path.size() > 6 && file_path.substr(file_path.size() - 6) == ".dylib") {
      alt_path = file_path.substr(0, file_path.size() - 6) + ".so";
    }
    if(!alt_path.empty()) {
      Logger::instance().info_f("[CODE_LOADER] Trying alternate extension: %s", alt_path.c_str());
      handle = dlopen(alt_path.c_str(), RTLD_LAZY);
    }
  }
  if(!handle) {
    const char* error = dlerror();
    Logger::instance().error_f("[CODE_LOADER] Failed to load %s: %s", file_path.c_str(),
                               error ? error : "unknown error");
    return nullptr;
  }

  Logger::instance().info_f("[CODE_LOADER] Successfully loaded: %s", file_path.c_str());
  return SoLibraryPtr(new SoLibraryHandle(handle));
}

SoFunction CodeLoader::get_so_function(const SoLibraryPtr& handle,
                                       const std::string&  function_name) {
  if(!handle || !handle->handle) {
    return nullptr;
  }

  dlerror();  // Clear any existing error
  void* func_ptr = dlsym(handle->handle, function_name.c_str());

  const char* error = dlerror();
  if(error) {
    Logger::instance().error_f("[CODE_LOADER] Function %s not found: %s", function_name.c_str(),
                               error);
    return nullptr;
  }

  return reinterpret_cast<SoFunction>(func_ptr);
}

SoFunctionV2 CodeLoader::get_so_function_v2(const SoLibraryPtr& handle,
                                            const std::string&  function_name) {
  if(!handle || !handle->handle) {
    return nullptr;
  }

  // First try the V2 function name (function_name_v2)
  std::string v2_name = function_name + "_v2";
  dlerror();  // Clear any existing error
  void* func_ptr = dlsym(handle->handle, v2_name.c_str());

  const char* error = dlerror();
  if(!error && func_ptr) {
    Logger::instance().info_f("[CODE_LOADER] Found V2 function: %s", v2_name.c_str());
    return reinterpret_cast<SoFunctionV2>(func_ptr);
  }

  // Try the original function name for V2 signature
  dlerror();
  func_ptr = dlsym(handle->handle, function_name.c_str());

  error = dlerror();
  if(error) {
    Logger::instance().debug_f("[CODE_LOADER] V2 function %s not found: %s",
                               function_name.c_str(), error);
    return nullptr;
  }

  // Note: We can't verify the signature at runtime, but the function exists
  // The caller should try V2 first, then fall back to V1
  return reinterpret_cast<SoFunctionV2>(func_ptr);
}

SoFunctionV3 CodeLoader::get_so_function_v3(const SoLibraryPtr& handle,
                                            const std::string&  function_name) {
  if(!handle || !handle->handle) {
    return nullptr;
  }

  // Try the V3 function name (function_name_v3)
  std::string v3_name = function_name + "_v3";
  dlerror();  // Clear any existing error
  void* func_ptr = dlsym(handle->handle, v3_name.c_str());

  const char* error = dlerror();
  if(!error && func_ptr) {
    Logger::instance().info_f("[CODE_LOADER] Found V3 function: %s", v3_name.c_str());
    return reinterpret_cast<SoFunctionV3>(func_ptr);
  }

  Logger::instance().debug_f("[CODE_LOADER] V3 function %s not found: %s",
                             v3_name.c_str(), error ? error : "null");
  return nullptr;
}

bool CodeLoader::unload_so_library(const std::string& file_path) {
  std::lock_guard<std::mutex> lock(so_cache_mutex_);

  auto it = so_library_cache_.find(file_path);
  if(it != so_library_cache_.end()) {
    Logger::instance().info_f("[CODE_LOADER] Unloading SO library: %s", file_path.c_str());
    so_library_cache_.erase(it);
    return true;
  }

  return false;
}

std::string CodeLoader::build_command_string(const std::string&              executable,
                                             const std::vector<std::string>& args) const {
  std::ostringstream oss;

  if(!executable.empty()) {
    oss << executable;
  }

  for(const auto& arg : args) {
    oss << " ";

    // Quote arguments containing spaces
    if(arg.find(' ') != std::string::npos) {
      oss << "\"" << arg << "\"";
    } else {
      oss << arg;
    }
  }

  return oss.str();
}

void CodeLoader::log_execution_start(const std::string& file_path, ExecutableType type) {
  Logger::instance().info_f("[CODE_EXEC_START] File=%s, Type=%s", file_path.c_str(),
                            executable_type_to_string(type).c_str());
}

void CodeLoader::log_execution_result(const std::string& file_path, const ExecutionResult& result) {
  if(result.success) {
    Logger::instance().info_f("[CODE_EXEC_SUCCESS] File=%s, Duration=%ldms, ExitCode=%d",
                              file_path.c_str(), result.duration_ms, result.exit_code);
  } else {
    Logger::instance().error_f("[CODE_EXEC_FAILED] File=%s, Duration=%ldms, ExitCode=%d, Error=%s",
                               file_path.c_str(), result.duration_ms, result.exit_code,
                               result.error_message.c_str());
  }
}

// ============================================================================
// Global code loader instance
// ============================================================================

namespace {
std::unique_ptr<CodeLoader> g_global_code_loader;
std::mutex                  g_global_loader_mutex;
}  // namespace

CodeLoader& global_code_loader() {
  std::lock_guard<std::mutex> lock(g_global_loader_mutex);

  if(!g_global_code_loader) {
    g_global_code_loader = std::make_unique<CodeLoader>();
  }

  return *g_global_code_loader;
}

// ============================================================================
// AgentFoundry 沙箱机制移植——安全层辅助方法（lld.md T028）
// exe_loader.h 声明：static resolve_safe_path(path, allowed_prefix)
// ============================================================================

// resolve_safe_path：路径双检（string prefix + realpath），拒绝越界（B4/FR-004）
// 算法：realpath 解析后再 prefix 检查（双检防符号链接逃逸）
// 异常：越界抛 std::runtime_error 含路径名与允许前缀
std::string CodeLoader::resolve_safe_path(const std::string& path,
                                          const std::string& allowed_prefix) {
  if (path.empty()) {
    throw std::runtime_error("Path escape rejected: empty path (allowed: '" + allowed_prefix + "')");
  }
  // 构造完整路径（相对路径基于 allowed_prefix 解析）
  std::string full = path;
  if (path[0] != '/') {
    full = allowed_prefix + "/" + path;
  }
  // realpath 解析符号链接（需 <cstdlib> 的 realpath，允 nullptr 返回新分配字符串）
  char* resolved = realpath(full.c_str(), nullptr);
  if (!resolved) {
    throw std::runtime_error("Path escape rejected: '" + path + "' (resolve failed, allowed: '" + allowed_prefix + "')");
  }
  std::string resolved_str(resolved);
  free(resolved);
  // 双检：resolved 必须以 allowed_prefix 开头（精确匹配或后跟 /）
  if (resolved_str != allowed_prefix &&
      !(resolved_str.size() > allowed_prefix.size() &&
        resolved_str.compare(0, allowed_prefix.size(), allowed_prefix) == 0 &&
        resolved_str[allowed_prefix.size()] == '/')) {
    throw std::runtime_error("Path escape rejected: '" + path + "' resolves to '" +
                            resolved_str + "' outside sandbox workdir '" + allowed_prefix + "'");
  }
  return resolved_str;
}

}  // namespace runtime
}  // namespace agentflow
