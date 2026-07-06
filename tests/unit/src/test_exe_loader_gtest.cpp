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
  // 不变量：成功命令必须立即返回，不得等到超时看门狗触发。
  // 阈值取 timeout_ms / 2 —— 既远小于 timeout_ms（能捕获"误等超时"的真实回归），
  // 又对一次性环境抖动（磁盘冷启动、Spotlight 索引等）有韧性。
  // 原硬编码 1500ms 在环境抖动下偶发 2332ms 误报，且与 timeout_ms 无显式关系。
  EXPECT_LT(elapsed.count(), config.timeout_ms / 2)
      << "命令应在超时看门狗触发前返回；elapsed=" << elapsed.count()
      << "ms, timeout_ms=" << config.timeout_ms;
}

TEST_F(CodeLoaderProcessTest, TimeoutTerminatesDescendantProcesses) {
  // 不变量：CodeLoader 超时杀进程组时，必须连后代进程一起清理。
  //
  // 验证策略：让脚本启动一个「带唯一标记」的长眠后代进程，超时后用 pgrep
  // 按标记检查该后代是否仍存活。完全不依赖 pid_file，消除一切写入/读取 race。
  //
  // 为什么不用 pid_file：高并发或系统负载下，shell 可能在被 SIGKILL 前根本
  // 没来得及执行 `echo $child > pid_file`，导致测试读到空文件而误判（实测
  // 4 并发下 ~75% 误报）。pgrep 方案直接查进程表，是 OS 真相源。
  const std::string marker = "agentflow_ut_sleep_" + std::to_string(::getpid()) +
                             "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  // 后代进程：用 sleep 配合一个会被 ps 显示的参数标记。
  // macOS/Linux 的 sleep 都接受浮点秒数；用 `sleep 30000` 足够长。
  // 标记通过 argv[0] 传递不可行（sleep 不接受），改用 shell 启动一个
  // 带 marker 名字的子 shell 跑 sleep，ps/pgrep -f 能匹配到命令行。
  const auto script = write_script(
      "spawn_child.sh",
      "bash -c 'exec -a \"" + marker + "\" sleep 30000' &\n"
      "wait $!\n");
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

  // 超时杀进程组后，等待 OS 回收并轮询确认后代已死。
  // pgrep -f 匹配完整命令行；唯一 marker 确保不误匹配其他进程。
  auto marker_alive = [&]() {
    std::string cmd = "pgrep -f '" + marker + "' 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return false;
    char buf[64];
    bool alive = (fgets(buf, sizeof(buf), pipe) != nullptr);
    ::pclose(pipe);
    return alive;
  };

  bool descendant_dead = false;
  for (int i = 0; i < 50; ++i) {
    if (!marker_alive()) {
      descendant_dead = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(descendant_dead) << "后代进程在超时后仍存活";

  // 兜底：如果测试自身失败残留了进程，清理掉避免污染后续测试。
  ::system(("pkill -f '" + marker + "' 2>/dev/null || true").c_str());
}

}  // namespace
