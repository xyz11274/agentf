#include "workflow/node/code_loader.h"

#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <sys/stat.h>

#include <sys/wait.h>

#include "utils/logger.h"

namespace agentflow {
namespace runtime {

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

CodeLoader::CodeLoader() {
  Logger::instance().info("[CODE_LOADER] CodeLoader initialized");
}

CodeLoader::~CodeLoader() {
  // Unload all cached SO libraries
  std::lock_guard<std::mutex> lock(so_cache_mutex_);

  for(auto& pair : so_library_cache_) {
    Logger::instance().info_f("[CODE_LOADER] Unloading: %s", pair.second.file_path.c_str());
  }

  so_library_cache_.clear();
  Logger::instance().info("[CODE_LOADER] CodeLoader destroyed");
}

ExecutionResult CodeLoader::load_and_execute_so(const std::string&     file_path,
                                                const std::string&     function_name,
                                                const ExecutionConfig& config) {
  auto start_time = std::chrono::steady_clock::now();

  log_execution_start(file_path, ExecutableType::SO_LIBRARY);

  ExecutionResult result;

  // Check if hot reload is enabled and we need to reload
  auto& hot_reload = global_hot_reload_manager();

  bool needs_reload = false;
  if(hot_reload.is_watching(file_path)) {
    auto current_version = get_loaded_version(file_path);
    auto latest_version  = hot_reload.get_current_version(file_path);

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

      auto version = hot_reload.get_current_version(file_path);
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
      std::string value_str = std::visit(
          [](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr(std::is_same_v<T, std::string>) {
              return arg;
            } else if constexpr(std::is_same_v<T, bool>) {
              return arg ? "true" : "false";
            } else {
              return std::to_string(arg);
            }
          },
          pair.second);

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
                                        const ExecutionConfig&          config) {
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
  return execute_command(command, config.working_dir, config.timeout_ms, config.env_vars);
}

ExecutionResult CodeLoader::execute_shell_script(const std::string&              script_path,
                                                 const std::vector<std::string>& args,
                                                 const ExecutionConfig&          config) {
  log_execution_start(script_path, ExecutableType::SHELL_SCRIPT);

  std::string command = build_command_string(script_path, args);
  return execute_command(command, config.working_dir, config.timeout_ms, config.env_vars);
}

ExecutionResult CodeLoader::execute_python_script(const std::string&              script_path,
                                                  const std::vector<std::string>& args,
                                                  const ExecutionConfig&          config) {
  log_execution_start(script_path, ExecutableType::PYTHON_SCRIPT);

  std::string python_cmd  = "python3";
  const char* python_path = std::getenv("PYTHON_PATH");
  std::string resolved_path = resolve_python_command(python_path);
  if(!resolved_path.empty()) {
    python_cmd = resolved_path;
  }

  std::string command = build_command_string(python_cmd, { script_path });
  command += " " + build_command_string("", args);

  return execute_command(command, config.working_dir, config.timeout_ms, config.env_vars);
}

ExecutionResult CodeLoader::execute(const std::string& file_path, const ExecutionConfig& config) {
  ExecutableType type = detect_executable_type(file_path);

  switch(type) {
    case ExecutableType::SO_LIBRARY:
      return load_and_execute_so(file_path, "execute", config);

    case ExecutableType::JAR_FILE:
      return execute_jar(file_path, "", {}, config);

    case ExecutableType::SHELL_SCRIPT:
      return execute_shell_script(file_path, {}, config);

    case ExecutableType::PYTHON_SCRIPT:
      return execute_python_script(file_path, {}, config);

    case ExecutableType::EXECUTABLE:
      return execute_shell_script(file_path, {}, config);

    default:
      ExecutionResult result;
      result.success       = false;
      result.error_message = "Unknown executable type";
      return result;
  }
}

bool CodeLoader::reload_code(const std::string& file_path) {
  auto& hot_reload = global_hot_reload_manager();

  if(!hot_reload.is_watching(file_path)) {
    hot_reload.start_watching(file_path);
  }

  auto reload_result = hot_reload.reload(file_path);
  return reload_result.success;
}

std::shared_ptr<ExecutableVersion> CodeLoader::get_loaded_version(
    const std::string& file_path) const {
  std::lock_guard<std::mutex> lock(so_cache_mutex_);

  auto it = so_library_cache_.find(file_path);
  if(it != so_library_cache_.end()) {
    auto& hot_reload = global_hot_reload_manager();
    return hot_reload.get_current_version(file_path);
  }

  return nullptr;
}

ExecutionResult CodeLoader::execute_command(const std::string& command,
                                            const std::string& working_dir, int timeout_ms,
                                            const std::vector<std::string>& env_vars) {
  ExecutionResult result;
  auto            start_time = std::chrono::steady_clock::now();

  Logger::instance().debug_f("[CODE_LOADER] Executing command: %s", command.c_str());

  // Use popen for simple execution
  FILE* pipe = popen(command.c_str(), "r");
  if(!pipe) {
    result.success       = false;
    result.error_message = "Failed to execute command";
    result.exit_code     = -1;
    return result;
  }

  // Read output
  char        buffer[4096];
  std::string output;
  while(fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }

  int status       = pclose(pipe);
  result.exit_code = WEXITSTATUS(status);

  result.stdout  = output;
  result.success = (result.exit_code == 0);

  if(!result.success) {
    result.error_message = "Command failed with exit code: " + std::to_string(result.exit_code);
    result.stderr        = output;
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

}  // namespace runtime
}  // namespace agentflow
