#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <signal.h>
#include <unistd.h>

#include "runtime/exe_loader.h"

namespace {

namespace fs = std::filesystem;
using agentflow::runtime::CodeLoader;
using agentflow::runtime::ExecutionConfig;

class CodeLoaderProcessTest : public ::testing::Test {
protected:
  void SetUp() override {
    temp_dir_ = fs::temp_directory_path() /
                ("agentflow_exe_loader_test_" + std::to_string(::getpid()));
    fs::create_directories(temp_dir_);
  }

  void TearDown() override { fs::remove_all(temp_dir_); }

  fs::path write_script(const std::string& name, const std::string& body) {
    fs::path path = temp_dir_ / name;
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body;
    out.close();
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
    return path;
  }

  fs::path temp_dir_;
};

TEST_F(CodeLoaderProcessTest, SuccessfulCommandReturnsWithoutWaitingForTimeoutWatchdog) {
  const auto script = write_script("quick.sh", "echo ready\n");
  ExecutionConfig config;
  config.timeout_ms = 5000;

  CodeLoader loader;
  const auto start = std::chrono::steady_clock::now();
  const auto result = loader.execute_shell_script(script.string(), {}, config);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.stdout, "ready\n");
  EXPECT_LT(elapsed.count(), 1500);
}

TEST_F(CodeLoaderProcessTest, TimeoutTerminatesDescendantProcesses) {
  const auto pid_file = temp_dir_ / "child.pid";
  const auto script = write_script(
      "spawn_child.sh",
      "sleep 30 &\n"
      "child=$!\n"
      "echo $child > '" + pid_file.string() + "'\n"
      "wait $child\n");
  ExecutionConfig config;
  config.timeout_ms = 1000;

  CodeLoader loader;
  const auto start = std::chrono::steady_clock::now();
  const auto result = loader.execute_shell_script(script.string(), {}, config);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  ASSERT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Command execution timeout");
  EXPECT_LT(elapsed.count(), 2500);

  std::ifstream in(pid_file);
  pid_t child_pid = -1;
  in >> child_pid;
  ASSERT_GT(child_pid, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(::kill(child_pid, 0), -1) << "descendant process survived timeout";
}

}  // namespace
