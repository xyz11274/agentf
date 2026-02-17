#ifndef AGENTFLOW_RUNTIME_CODE_LOADER_H
#define AGENTFLOW_RUNTIME_CODE_LOADER_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentflow/common.h"
#include "agentflow/runtime/hot_reload.h"

namespace agentflow {
namespace runtime {

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
   * Execute a Node.js script
   * @param script_path Path to the Node.js script
   * @param args Command line arguments
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult execute_nodejs_script(const std::string&              script_path,
                                        const std::vector<std::string>& args   = {},
                                        const ExecutionConfig&          config = ExecutionConfig());

  /**
   * Execute a Lua script
   * @param script_path Path to the Lua script
   * @param args Command line arguments
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult execute_lua_script(const std::string&              script_path,
                                     const std::vector<std::string>& args   = {},
                                     const ExecutionConfig&          config = ExecutionConfig());

  /**
   * Execute a Ruby script
   * @param script_path Path to the Ruby script
   * @param args Command line arguments
   * @param config Execution configuration
   * @return Execution result
   */
  ExecutionResult execute_ruby_script(const std::string&              script_path,
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
   * @return true if reload successful
   */
  bool reload_code(const std::string& file_path);

  /**
   * Get the current version info for a loaded file
   * @param file_path Path to the file
   * @return Version info or nullptr if not loaded
   */
  std::shared_ptr<ExecutableVersion> get_loaded_version(const std::string& file_path) const;

private:
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
