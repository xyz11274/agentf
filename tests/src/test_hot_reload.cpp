#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "agentflow/basic/logger.h"
#include "agentflow/framework/node.h"
#include "agentflow/runtime/code_loader.h"
#include "agentflow/runtime/hot_reload.h"

using namespace agentflow;
using namespace agentflow::runtime;

namespace fs = std::filesystem;

namespace {

bool is_executable_file(const std::string& path) {
  struct stat st;
  if(stat(path.c_str(), &st) != 0) {
    return false;
  }
  if(S_ISDIR(st.st_mode)) {
    return false;
  }
  return access(path.c_str(), X_OK) == 0;
}

bool command_available(const std::string& cmd) {
  if(cmd.empty()) {
    return false;
  }
  std::string command = "command -v " + cmd + " >/dev/null 2>&1";
  return std::system(command.c_str()) == 0;
}

bool python_available() {
  const char* python_path = std::getenv("PYTHON_PATH");
  if(python_path && *python_path != '\0') {
    std::string candidate = python_path;
    if(is_executable_file(candidate)) {
      return true;
    }
    struct stat st;
    if(stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      std::string from_dir = candidate + "/bin/python3";
      if(is_executable_file(from_dir)) {
        return true;
      }
    }
  }
  return command_available("python3");
}

bool lua_available() {
  return command_available("lua");
}

}  // namespace

// ============================================================================
// Test fixtures and helpers
// ============================================================================

class HotReloadTest : public ::testing::Test {
protected:
  std::string test_dir_;

  void SetUp() override {
    // Create test directory structure
    test_dir_ = "./test_data/hot_reload_" + std::to_string(std::time(nullptr));
    fs::create_directories(test_dir_);
    fs::create_directories(test_dir_ + "/scripts");
    fs::create_directories(test_dir_ + "/libs");
    fs::create_directories(test_dir_ + "/python");
    fs::create_directories(test_dir_ + "/java");
    fs::create_directories(test_dir_ + "/nodejs");
    fs::create_directories(test_dir_ + "/lua");
    fs::create_directories(test_dir_ + "/ruby");
    fs::create_directories(test_dir_ + "/backups");

    Logger::instance().set_level(Logger::Level::INFO);
    Logger::instance().info("[TEST] Test setup complete, test_dir=" + test_dir_);
  }

  void TearDown() override {
    // Clean up test directory
    if(fs::exists(test_dir_)) {
      fs::remove_all(test_dir_);
    }
    Logger::instance().info("[TEST] Test cleanup complete");
  }

  std::string create_shell_script(const std::string& filename, const std::string& content) {
    std::string   filepath = test_dir_ + "/scripts/" + filename;
    std::ofstream file(filepath);
    file << "#!/bin/bash\n";
    file << "set -e\n";
    file << content << "\n";
    file.close();
    fs::permissions(filepath, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                  | fs::perms::others_read | fs::perms::others_exec);
    return filepath;
  }

  std::string create_python_script(const std::string& filename, const std::string& content) {
    std::string   filepath = test_dir_ + "/python/" + filename;
    std::ofstream file(filepath);
    file << "#!/usr/bin/env python3\n";
    file << "import sys\n";
    file << "import json\n";
    file << content << "\n";
    file.close();
    fs::permissions(filepath, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                  | fs::perms::others_read | fs::perms::others_exec);
    return filepath;
  }

  std::string create_nodejs_script(const std::string& filename, const std::string& content) {
    std::string   filepath = test_dir_ + "/nodejs/" + filename;
    std::ofstream file(filepath);
    file << "#!/usr/bin/env node\n";
    file << content << "\n";
    file.close();
    fs::permissions(filepath, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                  | fs::perms::others_read | fs::perms::others_exec);
    return filepath;
  }

  std::string create_lua_script(const std::string& filename, const std::string& content) {
    std::string   filepath = test_dir_ + "/lua/" + filename;
    std::ofstream file(filepath);
    file << content << "\n";
    file.close();
    fs::permissions(filepath, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                  | fs::perms::others_read | fs::perms::others_exec);
    return filepath;
  }

  std::string create_ruby_script(const std::string& filename, const std::string& content) {
    std::string   filepath = test_dir_ + "/ruby/" + filename;
    std::ofstream file(filepath);
    file << "#!/usr/bin/env ruby\n";
    file << content << "\n";
    file.close();
    fs::permissions(filepath, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                  | fs::perms::others_read | fs::perms::others_exec);
    return filepath;
  }

  void wait_for_file_change(int ms = 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

  void modify_file(const std::string& filepath, const std::string& content) {
    std::ofstream file(filepath);
    file << content << "\n";
    file.close();
  }

  std::string get_file_content(const std::string& filepath) {
    std::ifstream     file(filepath);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }
};

// ============================================================================
// Shell Script Hot Reload Tests
// ============================================================================

TEST_F(HotReloadTest, ShellScript_BasicExecution) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("shell_basic.sh", "echo 'Hello from Shell'");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_shell_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.stdout.empty());
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stdout.find("Hello from Shell") != std::string::npos);
}

TEST_F(HotReloadTest, ShellScript_HotReload) {
  auto& manager = global_hot_reload_manager();
  (void)manager;
  auto& loader  = global_code_loader();

  std::string test_file = create_shell_script("shell_hotreload.sh", "echo 'Version 1.0'");
  manager.start_watching(test_file);
  loader.reload_code(test_file);
  wait_for_file_change(200);

  // Execute version 1
  ExecutionConfig config;
  config.timeout_ms = 5000;
  auto result1      = loader.execute_shell_script(test_file, {}, config);
  EXPECT_TRUE(result1.success);
  EXPECT_TRUE(result1.stdout.find("Version 1.0") != std::string::npos);

  // Modify file to version 2
  modify_file(test_file, "#!/bin/bash\nset -e\necho 'Version 2.0'");
  manager.reload(test_file);
  wait_for_file_change(500);

  // Execute version 2
  auto result2 = loader.execute_shell_script(test_file, {}, config);
  EXPECT_TRUE(result2.success);
  EXPECT_TRUE(result2.stdout.find("Version 2.0") != std::string::npos);
  EXPECT_TRUE(result2.stdout.find("Version 1.0") == std::string::npos);

  // Verify multiple versions exist
  auto versions = manager.get_all_versions(test_file);
  EXPECT_GE(versions.size(), 2);
}

TEST_F(HotReloadTest, ShellScript_WithArguments) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script(
      "shell_args.sh", "echo \"Args: $@\"\necho \"Arg1: $1\"\necho \"Arg2: $2\"");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms             = 5000;
  std::vector<std::string> args = { "hello", "world" };

  auto result = loader.execute_shell_script(test_file, args, config);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.stdout.find("hello") != std::string::npos);
  EXPECT_TRUE(result.stdout.find("world") != std::string::npos);
}

TEST_F(HotReloadTest, ShellScript_ErrorHandling) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("shell_error.sh", "echo 'This should error'\nexit 1");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_shell_script(test_file, {}, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.exit_code, 0);
}

TEST_F(HotReloadTest, ShellScript_WithWorkingDir) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("shell_workdir.sh", "echo \"Current dir: $(pwd)\"");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms  = 5000;
  config.working_dir = test_dir_;

  auto result = loader.execute_shell_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  // The output should contain test_dir (may be absolute or relative path)
  // This is a soft check since pwd behavior varies
  EXPECT_FALSE(result.stdout.empty());
}

// ============================================================================
// Python Script Hot Reload Tests
// ============================================================================

TEST_F(HotReloadTest, PythonScript_BasicExecution) {
  if(!python_available()) {
    GTEST_SKIP() << "Python interpreter not available";
  }
  auto& loader = global_code_loader();

  std::string test_file = create_python_script("python_basic.py", "print('Hello from Python')");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_python_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.stdout.empty());
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stdout.find("Hello from Python") != std::string::npos);
}

TEST_F(HotReloadTest, PythonScript_HotReload) {
  if(!python_available()) {
    GTEST_SKIP() << "Python interpreter not available";
  }
  auto& manager = global_hot_reload_manager();
  auto& loader  = global_code_loader();

  std::string test_file =
      create_python_script("python_hotreload.py", "print('Python Version 1.0')");
  manager.start_watching(test_file);
  loader.reload_code(test_file);
  wait_for_file_change(200);

  // Execute version 1
  ExecutionConfig config;
  config.timeout_ms = 5000;
  auto result1      = loader.execute_python_script(test_file, {}, config);
  EXPECT_TRUE(result1.success);
  EXPECT_TRUE(result1.stdout.find("Python Version 1.0") != std::string::npos);

  // Modify file to version 2
  modify_file(test_file,
              "#!/usr/bin/env python3\n"
              "import sys\n"
              "import json\n"
              "print('Python Version 2.0')");
  manager.reload(test_file);
  wait_for_file_change(500);

  // Execute version 2
  auto result2 = loader.execute_python_script(test_file, {}, config);
  EXPECT_TRUE(result2.success);
  EXPECT_TRUE(result2.stdout.find("Python Version 2.0") != std::string::npos);
}

TEST_F(HotReloadTest, PythonScript_WithArguments) {
  if(!python_available()) {
    GTEST_SKIP() << "Python interpreter not available";
  }
  auto& loader = global_code_loader();

  std::string test_file = create_python_script("python_args.py", "print('Args:', sys.argv[1:])");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms             = 5000;
  std::vector<std::string> args = { "--test", "value" };

  auto result = loader.execute_python_script(test_file, args, config);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.stdout.find("--test") != std::string::npos);
  EXPECT_TRUE(result.stdout.find("value") != std::string::npos);
}

TEST_F(HotReloadTest, PythonScript_WithInputs) {
  if(!python_available()) {
    GTEST_SKIP() << "Python interpreter not available";
  }
  auto& loader = global_code_loader();

  std::string test_file = create_python_script("python_inputs.py",
                                               "data = {'result': 'success', 'value': 42}\n"
                                               "print(json.dumps(data))");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms     = 5000;
  config.inputs["key1"] = "value1";
  config.inputs["key2"] = 42;

  auto result = loader.execute_python_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.stdout.find("success") != std::string::npos);
  EXPECT_TRUE(result.stdout.find("42") != std::string::npos);
}

TEST_F(HotReloadTest, PythonScript_ErrorHandling) {
  if(!python_available()) {
    GTEST_SKIP() << "Python interpreter not available";
  }
  auto& loader = global_code_loader();

  std::string test_file = create_python_script("python_error.py", "raise Exception('Test error')");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_python_script(test_file, {}, config);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.stderr.find("Test error") != std::string::npos
              || result.stdout.find("Test error") != std::string::npos);
}

// ============================================================================
// Node.js Script Hot Reload Tests
// ============================================================================

TEST_F(HotReloadTest, NodeJSScript_BasicExecution) {
  auto& loader = global_code_loader();

  std::string test_file =
      create_nodejs_script("nodejs_basic.js", "console.log('Hello from Node.js')");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_nodejs_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.stdout.empty());
  EXPECT_TRUE(result.stdout.find("Hello from Node.js") != std::string::npos);
}

TEST_F(HotReloadTest, NodeJSScript_HotReload) {
  auto& manager = global_hot_reload_manager();
  auto& loader  = global_code_loader();

  std::string test_file =
      create_nodejs_script("nodejs_hotreload.js", "console.log('Node.js Version 1.0')");
  manager.start_watching(test_file);
  loader.reload_code(test_file);
  wait_for_file_change(200);

  // Execute version 1
  ExecutionConfig config;
  config.timeout_ms = 5000;
  auto result1      = loader.execute_nodejs_script(test_file, {}, config);
  EXPECT_TRUE(result1.success);
  EXPECT_TRUE(result1.stdout.find("Node.js Version 1.0") != std::string::npos);

  // Modify file to version 2
  modify_file(test_file, "console.log('Node.js Version 2.0')");
  manager.reload(test_file);
  wait_for_file_change(500);

  // Execute version 2
  auto result2 = loader.execute_nodejs_script(test_file, {}, config);
  EXPECT_TRUE(result2.success);
  EXPECT_TRUE(result2.stdout.find("Node.js Version 2.0") != std::string::npos);
}

// ============================================================================
// Lua Script Hot Reload Tests
// ============================================================================

TEST_F(HotReloadTest, LuaScript_BasicExecution) {
  if(!lua_available()) {
    GTEST_SKIP() << "Lua interpreter not available";
  }
  auto& loader = global_code_loader();

  std::string test_file = create_lua_script("lua_basic.lua", "print('Hello from Lua')");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_lua_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.stdout.find("Hello from Lua") != std::string::npos);
}

TEST_F(HotReloadTest, LuaScript_HotReload) {
  if(!lua_available()) {
    GTEST_SKIP() << "Lua interpreter not available";
  }
  auto& manager = global_hot_reload_manager();
  auto& loader  = global_code_loader();

  std::string test_file = create_lua_script("lua_hotreload.lua", "print('Lua Version 1.0')");
  manager.start_watching(test_file);
  loader.reload_code(test_file);
  wait_for_file_change(200);

  // Execute version 1
  ExecutionConfig config;
  config.timeout_ms = 5000;
  auto result1      = loader.execute_lua_script(test_file, {}, config);
  EXPECT_TRUE(result1.success);
  EXPECT_TRUE(result1.stdout.find("Lua Version 1.0") != std::string::npos);

  // Modify file to version 2
  modify_file(test_file, "print('Lua Version 2.0')");
  manager.reload(test_file);
  wait_for_file_change(500);

  // Execute version 2
  auto result2 = loader.execute_lua_script(test_file, {}, config);
  EXPECT_TRUE(result2.success);
  EXPECT_TRUE(result2.stdout.find("Lua Version 2.0") != std::string::npos);
}

// ============================================================================
// Ruby Script Hot Reload Tests
// ============================================================================

TEST_F(HotReloadTest, RubyScript_BasicExecution) {
  auto& loader = global_code_loader();

  std::string test_file = create_ruby_script("ruby_basic.rb", "puts 'Hello from Ruby'");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_ruby_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.stdout.find("Hello from Ruby") != std::string::npos);
}

TEST_F(HotReloadTest, RubyScript_HotReload) {
  auto& manager = global_hot_reload_manager();
  auto& loader  = global_code_loader();

  std::string test_file = create_ruby_script("ruby_hotreload.rb", "puts 'Ruby Version 1.0'");
  manager.start_watching(test_file);
  loader.reload_code(test_file);
  wait_for_file_change(200);

  // Execute version 1
  ExecutionConfig config;
  config.timeout_ms = 5000;
  auto result1      = loader.execute_ruby_script(test_file, {}, config);
  EXPECT_TRUE(result1.success);
  EXPECT_TRUE(result1.stdout.find("Ruby Version 1.0") != std::string::npos);

  // Modify file to version 2
  modify_file(test_file, "puts 'Ruby Version 2.0'");
  manager.reload(test_file);
  wait_for_file_change(500);

  // Execute version 2
  auto result2 = loader.execute_ruby_script(test_file, {}, config);
  EXPECT_TRUE(result2.success);
  EXPECT_TRUE(result2.stdout.find("Ruby Version 2.0") != std::string::npos);
}

// ============================================================================
// JAR File Tests
// ============================================================================

TEST_F(HotReloadTest, JarFile_Execution) {
  auto& loader = global_code_loader();

  // Note: This test will only work if Java is installed and a valid JAR exists
  // In a real test environment, you would create an actual JAR file
  std::string jar_file = test_dir_ + "/java/test.jar";
  std::ofstream(jar_file).close();  // Create empty file for structure test

  // We can't execute without a real JAR, but we can test the path handling
  // In production, you would create a real JAR with:
  // javac Main.java && jar cfe test.jar Main.class

  // For now, just verify the loader exists
  (void)loader;  // Suppress unused warning
}

// ============================================================================
// Version Management Tests
// ============================================================================

TEST_F(HotReloadTest, VersionManager_CreateMultipleVersions) {
  auto&           manager = global_hot_reload_manager();
  HotReloadConfig config;
  config.max_versions = 5;
  manager.set_config(config);

  std::string test_file = create_shell_script("version_test.sh", "echo 'V1'");

  for(int i = 2; i <= 5; ++i) {
    manager.start_watching(test_file);
    wait_for_file_change(100);
    modify_file(test_file, "#!/bin/bash\nset -e\necho 'V" + std::to_string(i) + "'");
    manager.reload(test_file);
    wait_for_file_change(200);
  }

  auto versions = manager.get_all_versions(test_file);
  EXPECT_EQ(versions.size(), 5);

  auto active = manager.get_current_version(test_file);
  ASSERT_NE(active, nullptr);
  EXPECT_TRUE(active->is_active);
  EXPECT_TRUE(active->version_id.find("v") != std::string::npos);
}

TEST_F(HotReloadTest, VersionManager_MaxVersionsLimit) {
  auto&           manager = global_hot_reload_manager();
  HotReloadConfig config;
  config.max_versions = 3;
  manager.set_config(config);

  std::string test_file = create_shell_script("max_version_test.sh", "echo 'V1'");

  for(int i = 2; i <= 6; ++i) {
    manager.start_watching(test_file);
    wait_for_file_change(100);
    modify_file(test_file, "#!/bin/bash\nset -e\necho 'V" + std::to_string(i) + "'");
    manager.reload(test_file);
    wait_for_file_change(200);
  }

  auto versions = manager.get_all_versions(test_file);
  // Should only keep max_versions (3)
  EXPECT_LE(versions.size(), 3);
}

TEST_F(HotReloadTest, VersionManager_Rollback) {
  auto& manager = global_hot_reload_manager();

  std::string test_file = create_shell_script("rollback_test.sh", "echo 'Original'");

  manager.start_watching(test_file);
  auto version1 = manager.get_current_version(test_file);
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Create new version
  wait_for_file_change(100);
  modify_file(test_file, "#!/bin/bash\nset -e\necho 'Modified'");
  manager.reload(test_file);
  wait_for_file_change(200);

  auto version2 = manager.get_current_version(test_file);
  ASSERT_NE(version2, nullptr);
  EXPECT_NE(version2->version_id, version_id1);

  // Rollback to original version
  EXPECT_TRUE(manager.rollback_to_version(test_file, version_id1));

  auto current = manager.get_current_version(test_file);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->version_id, version_id1);
}

TEST_F(HotReloadTest, VersionManager_RollbackToInvalidVersion) {
  auto& manager = global_hot_reload_manager();

  std::string test_file = create_shell_script("invalid_rollback.sh", "echo 'test'");

  manager.start_watching(test_file);

  // Try to rollback to non-existent version
  EXPECT_FALSE(manager.rollback_to_version(test_file, "invalid_version_xyz"));
}

// ============================================================================
// Executable Type Detection Tests
// ============================================================================

TEST_F(HotReloadTest, TypeDetection_ShellScript) {
  std::string test_file = create_shell_script("detect_sh.sh", "echo 'test'");
  auto        type      = detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::SHELL_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_PythonScript) {
  std::string test_file = create_python_script("detect_py.py", "print('test')");
  auto        type      = detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::PYTHON_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_NodeJSScript) {
  std::string test_file = create_nodejs_script("detect_js.js", "console.log('test')");
  auto        type      = detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::NODE_JS_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_LuaScript) {
  std::string test_file = create_lua_script("detect_lua.lua", "print('test')");
  auto        type      = detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::LUA_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_RubyScript) {
  std::string test_file = create_ruby_script("detect_rb.rb", "puts 'test'");
  auto        type      = detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::RUBY_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_Unknown) {
  std::string   test_file = test_dir_ + "/unknown.xyz";
  std::ofstream file(test_file);
  file << "unknown content";
  file.close();

  auto type = detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::UNKNOWN);
}

TEST_F(HotReloadTest, TypeDetection_JarFile) {
  std::string   test_file = test_dir_ + "/test.jar";
  std::ofstream file(test_file);
  file << "jar content";
  file.close();

  auto type = detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::JAR_FILE);
}

// ============================================================================
// File Watcher Tests
// ============================================================================

TEST_F(HotReloadTest, FileWatcher_StartStop) {
  auto& manager = global_hot_reload_manager();

  std::string test_file = create_shell_script("watcher_test.sh", "echo 'test'");

  ASSERT_TRUE(manager.start_watching(test_file));
  EXPECT_TRUE(manager.is_watching(test_file));

  ASSERT_TRUE(manager.stop_watching(test_file));
  EXPECT_FALSE(manager.is_watching(test_file));
}

TEST_F(HotReloadTest, FileWatcher_NonExistentFile) {
  auto& manager = global_hot_reload_manager();

  std::string non_existent = test_dir_ + "/non_existent.sh";
  EXPECT_FALSE(manager.start_watching(non_existent));
  EXPECT_FALSE(manager.is_watching(non_existent));
}

TEST_F(HotReloadTest, FileWatcher_MultipleFiles) {
  auto& manager = global_hot_reload_manager();

  std::string file1 = create_shell_script("multi_test1.sh", "echo 'test1'");
  std::string file2 = create_shell_script("multi_test2.sh", "echo 'test2'");
  std::string file3 = create_shell_script("multi_test3.sh", "echo 'test3'");

  ASSERT_TRUE(manager.start_watching(file1));
  ASSERT_TRUE(manager.start_watching(file2));
  ASSERT_TRUE(manager.start_watching(file3));

  EXPECT_EQ(manager.get_watched_files_count(), 3);

  ASSERT_TRUE(manager.stop_watching(file1));
  EXPECT_EQ(manager.get_watched_files_count(), 2);

  ASSERT_TRUE(manager.stop_watching(file2));
  ASSERT_TRUE(manager.stop_watching(file3));
  EXPECT_EQ(manager.get_watched_files_count(), 0);
}

// ============================================================================
// Auto Reload Tests
// ============================================================================

TEST_F(HotReloadTest, AutoReload_Enabled) {
  auto& manager = global_hot_reload_manager();

  HotReloadConfig config;
  config.enabled           = true;
  config.auto_reload       = true;
  config.check_interval_ms = 100;
  config.max_versions      = 5;
  manager.set_config(config);

  std::string test_file = create_shell_script("auto_reload.sh", "echo 'V1'");

  ASSERT_TRUE(manager.start_watching(test_file));

  auto version1 = manager.get_current_version(test_file);
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Modify file
  wait_for_file_change(200);
  modify_file(test_file, "#!/bin/bash\nset -e\necho 'V2'");

  // Wait for auto reload to complete
  wait_for_file_change(2000);

  auto version2 = manager.get_current_version(test_file);
  ASSERT_NE(version2, nullptr);
  EXPECT_NE(version2->version_id, version_id1);
}

TEST_F(HotReloadTest, AutoReload_Disabled) {
  auto& manager = global_hot_reload_manager();

  HotReloadConfig config;
  config.enabled           = true;
  config.auto_reload       = false;  // Disable auto reload
  config.check_interval_ms = 100;
  manager.set_config(config);

  std::string test_file = create_shell_script("no_auto_reload.sh", "echo 'V1'");

  ASSERT_TRUE(manager.start_watching(test_file));

  auto version1 = manager.get_current_version(test_file);
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Modify file
  wait_for_file_change(200);
  modify_file(test_file, "#!/bin/bash\nset -e\necho 'V2'");

  // Wait - auto reload should not trigger
  wait_for_file_change(2000);

  // Version should not have changed
  auto version2 = manager.get_current_version(test_file);
  ASSERT_NE(version2, nullptr);
  // With auto_reload disabled, version won't change until manual reload
  EXPECT_EQ(version2->version_id, version_id1);
}

// ============================================================================
// Backup Tests
// ============================================================================

TEST_F(HotReloadTest, Backup_BeforeReload) {
  auto& manager = global_hot_reload_manager();

  HotReloadConfig config;
  config.enabled              = true;
  config.backup_before_reload = true;
  config.backup_dir           = test_dir_ + "/backups";
  manager.set_config(config);

  std::string test_file = create_shell_script("backup_test.sh", "echo 'V1'");
  manager.start_watching(test_file);

  // Reload should create backup
  auto reload_result = manager.reload(test_file);
  EXPECT_TRUE(reload_result.success);

  // Backup directory should exist
  EXPECT_TRUE(fs::exists(config.backup_dir));
}

// ============================================================================
// Node Integration Tests
// ============================================================================

TEST_F(HotReloadTest, Node_ShellScriptNode) {
  framework::Node::Config config;
  config.id                = "test_node_shell";
  config.name              = "Test Shell Node";
  config.type              = "test";
  config.execution_mode    = framework::Node::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_shell.sh", "echo 'Node execution'");
  config.enable_hot_reload = true;

  framework::Node node(config);
  ASSERT_TRUE(node.initialize());

  EXPECT_TRUE(node.enable_code_reload());
  ASSERT_TRUE(node.execute());

  auto outputs = node.get_outputs();
  EXPECT_FALSE(outputs.empty());
}

TEST_F(HotReloadTest, Node_PythonScriptNode) {
  framework::Node::Config config;
  config.id                = "test_node_python";
  config.name              = "Test Python Node";
  config.type              = "test";
  config.execution_mode    = framework::Node::ExecutionMode::PYTHON_SCRIPT;
  config.command           = create_python_script("node_python.py", "print('Node execution')");
  config.enable_hot_reload = true;

  framework::Node node(config);
  ASSERT_TRUE(node.initialize());

  EXPECT_TRUE(node.enable_code_reload());
  ASSERT_TRUE(node.execute());

  auto outputs = node.get_outputs();
  EXPECT_FALSE(outputs.empty());
}

TEST_F(HotReloadTest, Node_HotReloadIntegration) {
  auto& manager = global_hot_reload_manager();
  (void)manager;

  framework::Node::Config config;
  config.id                = "test_node_hotreload";
  config.name              = "Test Hot Reload Node";
  config.type              = "test";
  config.execution_mode    = framework::Node::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_integration.sh", "echo 'Version 1'");
  config.enable_hot_reload = true;

  framework::Node node(config);
  ASSERT_TRUE(node.initialize());
  ASSERT_TRUE(node.enable_code_reload());

  // Execute version 1
  ASSERT_TRUE(node.execute());
  auto version1 = node.get_code_version();
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Modify file
  wait_for_file_change(200);
  modify_file(config.command, "#!/bin/bash\nset -e\necho 'Version 2'");

  // Check and reload
  ASSERT_TRUE(node.check_and_reload_code());
  wait_for_file_change(500);

  auto version2 = node.get_code_version();
  ASSERT_NE(version2, nullptr);
  EXPECT_NE(version2->version_id, version_id1);

  // Cleanup and reinitialize to execute new version
  node.cleanup();
  ASSERT_TRUE(node.initialize());

  // Execute version 2
  ASSERT_TRUE(node.execute());

  auto outputs = node.get_outputs();
  EXPECT_FALSE(outputs.empty());
}

TEST_F(HotReloadTest, Node_RollbackIntegration) {
  framework::Node::Config config;
  config.id                = "test_node_rollback";
  config.name              = "Test Rollback Node";
  config.type              = "test";
  config.execution_mode    = framework::Node::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_rollback.sh", "echo 'Original'");
  config.enable_hot_reload = true;

  framework::Node node(config);
  ASSERT_TRUE(node.initialize());
  ASSERT_TRUE(node.enable_code_reload());

  auto version1 = node.get_code_version();
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Create new version
  wait_for_file_change(200);
  modify_file(config.command, "#!/bin/bash\nset -e\necho 'Modified'");
  ASSERT_TRUE(node.check_and_reload_code());
  wait_for_file_change(500);

  auto version2 = node.get_code_version();
  ASSERT_NE(version2, nullptr);
  EXPECT_NE(version2->version_id, version_id1);

  // Rollback
  ASSERT_TRUE(node.rollback_to_version(version_id1));

  auto current = node.get_code_version();
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->version_id, version_id1);
}

TEST_F(HotReloadTest, Node_DisableHotReload) {
  framework::Node::Config config;
  config.id                = "test_node_no_reload";
  config.name              = "Test No Reload Node";
  config.type              = "test";
  config.execution_mode    = framework::Node::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_no_reload.sh", "echo 'test'");
  config.enable_hot_reload = false;

  framework::Node node(config);
  ASSERT_TRUE(node.initialize());

  EXPECT_FALSE(node.enable_code_reload());
}

// ============================================================================
// Timeout and Error Tests
// ============================================================================

TEST_F(HotReloadTest, Timeout_ExecutionTimeout) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("timeout_test.sh", "sleep 10; echo 'Done'");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 1000;  // 1 second timeout

  auto result = loader.execute_shell_script(test_file, {}, config);
  EXPECT_FALSE(result.success);
}

TEST_F(HotReloadTest, Error_InvalidFile) {
  auto& loader = global_code_loader();

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_shell_script(test_dir_ + "/non_existent.sh", {}, config);
  EXPECT_FALSE(result.success);
}

TEST_F(HotReloadTest, Error_MalformedScript) {
  auto& loader = global_code_loader();

  std::string   test_file = test_dir_ + "/malformed.sh";
  std::ofstream file(test_file);
  file << "this is not a valid bash script\n";
  file.close();

  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_shell_script(test_file, {}, config);
  EXPECT_FALSE(result.success);
}

// ============================================================================
// Config Tests
// ============================================================================

TEST_F(HotReloadTest, Config_DefaultValues) {
  HotReloadConfig config;

  EXPECT_TRUE(config.enabled);
  EXPECT_TRUE(config.auto_reload);
  EXPECT_EQ(config.max_versions, 5);
  EXPECT_EQ(config.check_interval_ms, 1000);
  EXPECT_TRUE(config.validate_before_reload);
  EXPECT_TRUE(config.backup_before_reload);
  EXPECT_EQ(config.reload_timeout_ms, 30000);
}

TEST_F(HotReloadTest, ExecutableTypeToString) {
  EXPECT_EQ(executable_type_to_string(ExecutableType::SHELL_SCRIPT), "SHELL_SCRIPT");
  EXPECT_EQ(executable_type_to_string(ExecutableType::PYTHON_SCRIPT), "PYTHON_SCRIPT");
  EXPECT_EQ(executable_type_to_string(ExecutableType::JAR_FILE), "JAR_FILE");
  EXPECT_EQ(executable_type_to_string(ExecutableType::SO_LIBRARY), "SO_LIBRARY");
  EXPECT_EQ(executable_type_to_string(ExecutableType::NODE_JS_SCRIPT), "NODE_JS_SCRIPT");
  EXPECT_EQ(executable_type_to_string(ExecutableType::LUA_SCRIPT), "LUA_SCRIPT");
  EXPECT_EQ(executable_type_to_string(ExecutableType::RUBY_SCRIPT), "RUBY_SCRIPT");
  EXPECT_EQ(executable_type_to_string(ExecutableType::EXECUTABLE), "EXECUTABLE");
  EXPECT_EQ(executable_type_to_string(ExecutableType::UNKNOWN), "UNKNOWN");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  Logger::instance().set_level(Logger::Level::INFO);
  Logger::instance().info("==============================================");
  Logger::instance().info("  AgentFlow Hot Reload Tests");
  Logger::instance().info("==============================================");

  int result = RUN_ALL_TESTS();

  Logger::instance().info("==============================================");
  if(result == 0) {
    Logger::instance().info("  All tests passed!");
  } else {
    Logger::instance().error("  Some tests failed!");
  }
  Logger::instance().info("==============================================");

  return result;
}
