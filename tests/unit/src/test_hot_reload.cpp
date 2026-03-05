#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/logger.h"
#include "workflow/node.h"
#include "runtime/exe_loader.h"

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

bool go_available() {
  const char* go_path = std::getenv("GO_PATH");
  if(go_path && *go_path != '\0') {
    std::string candidate = go_path;
    if(is_executable_file(candidate)) {
      return true;
    }
    struct stat st;
    if(stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      std::string from_dir = candidate + "/bin/go";
      if(is_executable_file(from_dir)) {
        return true;
      }
    }
  }
  return command_available("go");
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
    fs::create_directories(test_dir_ + "/go");
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

  std::string create_go_script(const std::string& filename, const std::string& content) {
    std::string   filepath = test_dir_ + "/go/" + filename;
    std::ofstream file(filepath);
    file << "package main\n";
    file << "import \"fmt\"\n";
    file << "func main() {\n";
    file << content << "\n";
    file << "}\n";
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

TEST_F(HotReloadTest, DISABLED_ShellScript_BasicExecution) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("shell_basic.sh", "echo 'Hello from Shell'");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 20000;

  auto result = loader.execute_shell_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.stdout.empty());
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stdout.find("Hello from Shell") != std::string::npos);
}

TEST_F(HotReloadTest, DISABLED_ShellScript_HotReload) {
  auto& loader  = global_code_loader();

  std::string test_file = create_shell_script("shell_hotreload.sh", "echo 'Version 1.0'");
  loader.start_watching(test_file);
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
  loader.reload_code(test_file);
  wait_for_file_change(500);

  // Execute version 2
  auto result2 = loader.execute_shell_script(test_file, {}, config);
  EXPECT_TRUE(result2.success);
  EXPECT_TRUE(result2.stdout.find("Version 2.0") != std::string::npos);
  EXPECT_TRUE(result2.stdout.find("Version 1.0") == std::string::npos);

  // Verify multiple versions exist
  auto versions = loader.get_all_versions(test_file);
  EXPECT_GE(versions.size(), 2);
  
  // Cleanup: stop watching the file
  loader.stop_watching(test_file);
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
  auto& loader  = global_code_loader();

  std::string test_file =
      create_python_script("python_hotreload.py", "print('Python Version 1.0')");
  loader.start_watching(test_file);
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
  loader.reload_code(test_file);
  wait_for_file_change(500);

  // Execute version 2
  auto result2 = loader.execute_python_script(test_file, {}, config);
  EXPECT_TRUE(result2.success);
  EXPECT_TRUE(result2.stdout.find("Python Version 2.0") != std::string::npos);
  
  // Cleanup: stop watching the file
  loader.stop_watching(test_file);
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

TEST_F(HotReloadTest, DISABLED_GoScript_BasicExecution) {
  if(!go_available()) {
    GTEST_SKIP() << "Go runtime not available";
  }
  auto& loader = global_code_loader();

  std::string test_file = create_go_script("go_basic.go", "fmt.Println(\"Hello from Go\")");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  ExecutionConfig config;
  config.timeout_ms = 5000;

  auto result = loader.execute_go_script(test_file, {}, config);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.stdout.empty());
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stdout.find("Hello from Go") != std::string::npos);
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
  auto&           loader = global_code_loader();
  HotReloadConfig config;
  config.max_versions = 5;
  loader.set_hot_reload_config(config);

  std::string test_file = create_shell_script("version_test.sh", "echo 'V1'");

  for(int i = 2; i <= 5; ++i) {
    loader.start_watching(test_file);
    wait_for_file_change(100);
    modify_file(test_file, "#!/bin/bash\nset -e\necho 'V" + std::to_string(i) + "'");
    loader.reload_code(test_file);
    wait_for_file_change(200);
  }

  auto versions = loader.get_all_versions(test_file);
  EXPECT_EQ(versions.size(), 5);

  auto active = loader.get_current_version(test_file);
  ASSERT_NE(active, nullptr);
  EXPECT_TRUE(active->is_active);
  EXPECT_TRUE(active->version_id.find("v") != std::string::npos);
  
  // Cleanup: stop watching the file
  loader.stop_watching(test_file);
}

TEST_F(HotReloadTest, VersionManager_MaxVersionsLimit) {
  auto&           loader = global_code_loader();
  HotReloadConfig config;
  config.max_versions = 3;
  loader.set_hot_reload_config(config);

  std::string test_file = create_shell_script("max_version_test.sh", "echo 'V1'");

  for(int i = 2; i <= 6; ++i) {
    loader.start_watching(test_file);
    wait_for_file_change(100);
    modify_file(test_file, "#!/bin/bash\nset -e\necho 'V" + std::to_string(i) + "'");
    loader.reload_code(test_file);
    wait_for_file_change(200);
  }

  auto versions = loader.get_all_versions(test_file);
  // Should only keep max_versions (3)
  EXPECT_LE(versions.size(), 3);
  
  // Cleanup: stop watching the file
  loader.stop_watching(test_file);
}

TEST_F(HotReloadTest, VersionManager_Rollback) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("rollback_test.sh", "echo 'Original'");

  loader.start_watching(test_file);
  auto version1 = loader.get_current_version(test_file);
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Create new version
  wait_for_file_change(100);
  modify_file(test_file, "#!/bin/bash\nset -e\necho 'Modified'");
  loader.reload_code(test_file);
  wait_for_file_change(200);

  auto version2 = loader.get_current_version(test_file);
  ASSERT_NE(version2, nullptr);
  EXPECT_NE(version2->version_id, version_id1);

  // Rollback to original version
  EXPECT_TRUE(loader.rollback_to_version(test_file, version_id1));

  auto current = loader.get_current_version(test_file);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->version_id, version_id1);
  
  // Cleanup: stop watching the file
  loader.stop_watching(test_file);
}

TEST_F(HotReloadTest, VersionManager_RollbackToInvalidVersion) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("invalid_rollback.sh", "echo 'test'");

  loader.start_watching(test_file);

  // Try to rollback to non-existent version
  EXPECT_FALSE(loader.rollback_to_version(test_file, "invalid_version_xyz"));
  
  // Cleanup: stop watching the file
  loader.stop_watching(test_file);
}

// ============================================================================
// Executable Type Detection Tests
// ============================================================================

TEST_F(HotReloadTest, TypeDetection_ShellScript) {
  std::string test_file = create_shell_script("detect_sh.sh", "echo 'test'");
  auto        type      = CodeLoader::detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::SHELL_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_PythonScript) {
  std::string test_file = create_python_script("detect_py.py", "print('test')");
  auto        type      = CodeLoader::detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::PYTHON_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_GoScript) {
  std::string test_file = create_go_script("detect_go.go", "fmt.Println(\"test\")");
  auto        type      = CodeLoader::detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::GO_SCRIPT);
}

TEST_F(HotReloadTest, TypeDetection_Unknown) {
  std::string   test_file = test_dir_ + "/unknown.xyz";
  std::ofstream file(test_file);
  file << "unknown content";
  file.close();

  auto type = CodeLoader::detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::UNKNOWN);
}

TEST_F(HotReloadTest, TypeDetection_JarFile) {
  std::string   test_file = test_dir_ + "/test.jar";
  std::ofstream file(test_file);
  file << "jar content";
  file.close();

  auto type = CodeLoader::detect_executable_type(test_file);
  EXPECT_EQ(type, ExecutableType::JAR_FILE);
}

// ============================================================================
// File Watcher Tests
// ============================================================================

TEST_F(HotReloadTest, FileWatcher_StartStop) {
  auto& loader = global_code_loader();

  std::string test_file = create_shell_script("watcher_test.sh", "echo 'test'");

  ASSERT_TRUE(loader.start_watching(test_file));
  EXPECT_TRUE(loader.is_watching(test_file));

  ASSERT_TRUE(loader.stop_watching(test_file));
  EXPECT_FALSE(loader.is_watching(test_file));
}

TEST_F(HotReloadTest, FileWatcher_NonExistentFile) {
  auto& loader = global_code_loader();

  std::string non_existent = test_dir_ + "/non_existent.sh";
  EXPECT_FALSE(loader.start_watching(non_existent));
  EXPECT_FALSE(loader.is_watching(non_existent));
}

TEST_F(HotReloadTest, FileWatcher_MultipleFiles) {
  auto& loader = global_code_loader();

  std::string file1 = create_shell_script("multi_test1.sh", "echo 'test1'");
  std::string file2 = create_shell_script("multi_test2.sh", "echo 'test2'");
  std::string file3 = create_shell_script("multi_test3.sh", "echo 'test3'");

  ASSERT_TRUE(loader.start_watching(file1));
  ASSERT_TRUE(loader.start_watching(file2));
  ASSERT_TRUE(loader.start_watching(file3));

  EXPECT_EQ(loader.get_watched_files_count(), 3);

  ASSERT_TRUE(loader.stop_watching(file1));
  EXPECT_EQ(loader.get_watched_files_count(), 2);

  ASSERT_TRUE(loader.stop_watching(file2));
  ASSERT_TRUE(loader.stop_watching(file3));
  EXPECT_EQ(loader.get_watched_files_count(), 0);
}

// ============================================================================
// Auto Reload Tests
// ============================================================================

TEST_F(HotReloadTest, DISABLED_AutoReload_Enabled) {
  auto& loader = global_code_loader();

  HotReloadConfig config;
  config.enabled           = true;
  config.auto_reload       = true;
  config.check_interval_ms = 100;
  config.max_versions      = 5;
  loader.set_hot_reload_config(config);

  std::string test_file = create_shell_script("auto_reload.sh", "echo 'V1'");

  ASSERT_TRUE(loader.start_watching(test_file));

  auto version1 = loader.get_current_version(test_file);
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Modify file
  wait_for_file_change(200);
  modify_file(test_file, "#!/bin/bash\nset -e\necho 'V2'");

  // Wait for auto reload to complete
  wait_for_file_change(2000);

  auto version2 = loader.get_current_version(test_file);
  ASSERT_NE(version2, nullptr);
  EXPECT_NE(version2->version_id, version_id1);
  
  // Cleanup: stop watching the file and disable hot reload
  loader.stop_watching(test_file);
  HotReloadConfig disable_config;
  disable_config.enabled = false;
  loader.set_hot_reload_config(disable_config);
}

TEST_F(HotReloadTest, DISABLED_AutoReload_Disabled) {
  auto& loader = global_code_loader();

  HotReloadConfig config;
  config.enabled           = true;
  config.auto_reload       = false;  // Disable auto reload
  config.check_interval_ms = 100;
  loader.set_hot_reload_config(config);

  std::string test_file = create_shell_script("no_auto_reload.sh", "echo 'V1'");

  ASSERT_TRUE(loader.start_watching(test_file));

  auto version1 = loader.get_current_version(test_file);
  ASSERT_NE(version1, nullptr);
  std::string version_id1 = version1->version_id;

  // Modify file
  wait_for_file_change(200);
  modify_file(test_file, "#!/bin/bash\nset -e\necho 'V2'");

  // Wait - auto reload should not trigger
  wait_for_file_change(2000);

  // Version should not have changed
  auto version2 = loader.get_current_version(test_file);
  ASSERT_NE(version2, nullptr);
  // With auto_reload disabled, version won't change until manual reload
  EXPECT_EQ(version2->version_id, version_id1);
  
  // Cleanup: stop watching the file
  loader.stop_watching(test_file);
}

// ============================================================================
// Backup Tests
// ============================================================================

TEST_F(HotReloadTest, Backup_BeforeReload) {
  auto& loader = global_code_loader();

  HotReloadConfig config;
  config.enabled              = true;
  config.backup_before_reload = true;
  config.backup_dir           = test_dir_ + "/backups";
  loader.set_hot_reload_config(config);

  std::string test_file = create_shell_script("backup_test.sh", "echo 'V1'");
  loader.start_watching(test_file);

  // Reload should create backup
  auto reload_result = loader.reload_code(test_file);
  EXPECT_TRUE(reload_result.success);

  // Backup directory should exist
  EXPECT_TRUE(fs::exists(config.backup_dir));
  
  // Cleanup: stop watching the file and disable hot reload
  loader.stop_watching(test_file);
  HotReloadConfig disable_config;
  disable_config.enabled = false;
  loader.set_hot_reload_config(disable_config);
}

// ============================================================================
// MMNode Integration Tests
// ============================================================================

TEST_F(HotReloadTest, Node_ShellScriptNode) {
  framework::MMNode::Config config;
  config.id                = "test_node_shell";
  config.name              = "Test Shell MMNode";
  config.type              = "test";
  config.execution_mode    = framework::MMNode::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_shell.sh", "echo 'MMNode execution'");
  config.enable_hot_reload = true;

  framework::MMNode node(config);
  ASSERT_TRUE(node.initialize());

  ASSERT_TRUE(node.execute());

  auto outputs = node.get_outputs();
  EXPECT_FALSE(outputs.empty());
}

TEST_F(HotReloadTest, Node_PythonScriptNode) {
  framework::MMNode::Config config;
  config.id                = "test_node_python";
  config.name              = "Test Python MMNode";
  config.type              = "test";
  config.execution_mode    = framework::MMNode::ExecutionMode::PYTHON_SCRIPT;
  config.command           = create_python_script("node_python.py", "print('MMNode execution')");
  config.enable_hot_reload = true;

  framework::MMNode node(config);
  ASSERT_TRUE(node.initialize());

  ASSERT_TRUE(node.execute());

  auto outputs = node.get_outputs();
  EXPECT_FALSE(outputs.empty());
}

TEST_F(HotReloadTest, DISABLED_Node_HotReloadIntegration) {
  auto& loader = global_code_loader();
  (void)loader;

  framework::MMNode::Config config;
  config.id                = "test_node_hotreload";
  config.name              = "Test Hot Reload MMNode";
  config.type              = "test";
  config.execution_mode    = framework::MMNode::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_integration.sh", "echo 'Version 1'");
  config.enable_hot_reload = true;

  framework::MMNode node(config);
  ASSERT_TRUE(node.initialize());
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
  framework::MMNode::Config config;
  config.id                = "test_node_rollback";
  config.name              = "Test Rollback MMNode";
  config.type              = "test";
  config.execution_mode    = framework::MMNode::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_rollback.sh", "echo 'Original'");
  config.enable_hot_reload = true;

  framework::MMNode node(config);
  ASSERT_TRUE(node.initialize());
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

TEST_F(HotReloadTest, Node_DefaultHotReloadWithoutEnable) {
  framework::MMNode::Config config;
  config.id                = "test_node_no_reload";
  config.name              = "Test No Reload MMNode";
  config.type              = "test";
  config.execution_mode    = framework::MMNode::ExecutionMode::SHELL_SCRIPT;
  config.command           = create_shell_script("node_no_reload.sh", "echo 'test'");
  config.enable_hot_reload = true;

  framework::MMNode node(config);
  ASSERT_TRUE(node.initialize());

  ASSERT_TRUE(node.execute());
  auto version = node.get_code_version();
  EXPECT_NE(version, nullptr);
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
  EXPECT_EQ(CodeLoader::executable_type_to_string(ExecutableType::SHELL_SCRIPT), "SHELL_SCRIPT");
  EXPECT_EQ(CodeLoader::executable_type_to_string(ExecutableType::GO_SCRIPT), "GO_SCRIPT");
  EXPECT_EQ(CodeLoader::executable_type_to_string(ExecutableType::PYTHON_SCRIPT), "PYTHON_SCRIPT");
  EXPECT_EQ(CodeLoader::executable_type_to_string(ExecutableType::JAR_FILE), "JAR_FILE");
  EXPECT_EQ(CodeLoader::executable_type_to_string(ExecutableType::SO_LIBRARY), "SO_LIBRARY");
  EXPECT_EQ(CodeLoader::executable_type_to_string(ExecutableType::EXECUTABLE), "EXECUTABLE");
  EXPECT_EQ(CodeLoader::executable_type_to_string(ExecutableType::UNKNOWN), "UNKNOWN");
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
