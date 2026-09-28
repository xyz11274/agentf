// 宿主进程监督：受控传递环境、限制资源、等待退出并处理超时与取消。
// 本组件负责进程生命周期，不把用户可执行程序当成安全沙箱。
// 清理只针对本次启动的进程树，避免影响并行任务或主 Agent。

// 子进程监管独立于业务判分，负责超时、取消和资源边界。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "evaluation_process.h"

#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

namespace trajectory::evolution {
namespace {
using Clock = std::chrono::steady_clock;

struct Descriptors {
  std::array<int, 8> value{{-1, -1, -1, -1, -1, -1, -1, -1}};

  // 关闭指定文件描述符并更新持有状态，避免重复关闭误伤被系统复用的描述符。
  void close_at(int index) {
    if (value[index] >= 0) {
      ::close(value[index]);
      value[index] = -1;
    }
  }

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~Descriptors() {
    for (int i = 0; i < 8; ++i)
      close_at(i);
  }
};

struct Child {
  pid_t pid;
  bool reaped = false;

  // 请求停止当前工作并完成资源收尾；不将停止信号解释为业务成功。
  bool stop(int &status) {
    if (pid <= 0 || reaped)
      return true;
    ::kill(pid, SIGTERM);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    do {
      const auto waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        reaped = true;
        return true;
      }
      if (waited < 0 && errno != EINTR) {
        reaped = true;
        return false;
      }
      ::poll(nullptr, 0, 10);
    } while (Clock::now() < deadline);
    ::kill(-pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    reaped = true;
    return false;
  }

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~Child() {
    int status = 0;
    stop(status);
  }
};

// 定位包内评测监督进程，避免要求用户另写框架专用启动器。
std::string supervisor_executable() {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidates;
  std::error_code error;
  const auto executable = fs::read_symlink("/proc/self/exe", error);
  if (!error)
    candidates.push_back(executable.parent_path() / "trajectory_evolution");
  static int module_anchor;
  Dl_info module{};
  if (::dladdr(&module_anchor, &module) && module.dli_fname) {
    const auto directory = fs::absolute(module.dli_fname).parent_path();
    candidates.push_back(directory / "trajectory_evolution");
    candidates.push_back(directory.parent_path() / "bin/trajectory_evolution");
  }
  candidates.emplace_back(EVOLUTION_INSTALLED_CLI);
  candidates.emplace_back(EVOLUTION_BUILD_CLI);
  for (const auto &candidate : candidates) {
    error.clear();
    if (fs::is_regular_file(candidate, error) && !error && ::access(candidate.c_str(), X_OK) == 0)
      return candidate.string();
  }
  return {};
}
}

// 在隔离监督进程中驱动宿主，统一管理超时、取消和子进程回收。
// 派发进程退出与真实任务完成分开判断，异步分支仍需通过采集终态确认。
EvaluationProcessResult run_evaluation_process(
    const std::vector<std::string> &command, const std::vector<std::string> &environment,
    const std::filesystem::path &directory, const std::string &input, std::size_t timeout_ms,
    std::size_t max_output_bytes, const std::function<bool()> &cancelled,
    const EvaluationProcessLimits &limits, const std::function<bool()> &capture_finished,
    const std::function<std::string()> &entry_status) {
  EvaluationProcessResult result;
  const auto started = Clock::now();
  const auto supervisor = supervisor_executable();
  if (supervisor.empty()) {
    result.error_code = "runner_supervisor_not_found";
    return result;
  }
  Descriptors fds;
  auto &fd = fds.value;
  if (command.empty() || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd.data()) ||
      ::pipe2(fd.data() + 2, O_CLOEXEC) || ::pipe2(fd.data() + 4, O_CLOEXEC) ||
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd.data() + 6)) {
    result.error_code = "runner_pipe_failed";
    return result;
  }
  std::vector<char *> argv, envp;
  std::vector<std::string> supervised{supervisor, "--internal-evaluation-supervisor",
                                      std::to_string(::getpid())};
  supervised.insert(supervised.end(), command.begin(), command.end());
  for (const auto &item : supervised)
    argv.push_back(const_cast<char *>(item.c_str()));
  for (const auto &item : environment)
    envp.push_back(const_cast<char *>(item.c_str()));
  argv.push_back(nullptr);
  envp.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  if (posix_spawn_file_actions_init(&actions)) {
    result.error_code = "runner_spawn_failed";
    return result;
  }
  if (posix_spawnattr_init(&attributes)) {
    posix_spawn_file_actions_destroy(&actions);
    result.error_code = "runner_spawn_failed";
    return result;
  }
  int error = posix_spawn_file_actions_adddup2(&actions, fd[1], STDIN_FILENO);
  error |= posix_spawn_file_actions_adddup2(&actions, fd[3], STDOUT_FILENO);
  error |= posix_spawn_file_actions_adddup2(&actions, fd[5], STDERR_FILENO);
  // 文件描述符 3 是私有启动门；监督进程在 Agent 启动前将其关闭。
  // 服务锁和其他描述符不能进入试验进程。
  error |= posix_spawn_file_actions_adddup2(&actions, fd[6], 3);
  error |= posix_spawn_file_actions_addclosefrom_np(&actions, 4);
  error |= posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  error |= posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  error |= posix_spawnattr_setpgroup(&attributes, 0);
  pid_t pid = -1;
  if (!error)
    error = posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (error) {
    result.error_code = "runner_spawn_failed";
    return result;
  }
  Child child{pid};
  // 先设置资源限制，再发送任务，后代进程继承这些限制。
  // 部署层还可用容器或控制组限制整个进程树。
  bool limited = true;
  if (limits.nice)
    limited &= ::setpriority(PRIO_PROCESS, pid, limits.nice) == 0;
#if defined(__linux__)
  if (limits.address_space_bytes) {
    const rlimit bound{limits.address_space_bytes, limits.address_space_bytes};
    limited &= ::prlimit(pid, RLIMIT_AS, &bound, nullptr) == 0;
  }
  if (limits.cpu_seconds) {
    const rlimit bound{limits.cpu_seconds, limits.cpu_seconds};
    limited &= ::prlimit(pid, RLIMIT_CPU, &bound, nullptr) == 0;
  }
#else
  limited &= !limits.address_space_bytes && !limits.cpu_seconds;
#endif
  if (!limited) {
    result.error_code = "runner_resource_limits_failed";
    return result;
  }
  if (::send(fd[7], "G", 1, MSG_NOSIGNAL) != 1) {
    result.error_code = "runner_supervisor_start_failed";
    return result;
  }
  fds.close_at(6);
  fds.close_at(7);
  fds.close_at(1);
  fds.close_at(3);
  fds.close_at(5);
  for (int index : {0, 2, 4})
    if (fcntl(fd[index], F_SETFL, fcntl(fd[index], F_GETFL) | O_NONBLOCK) < 0)
      result.error_code = "runner_pipe_failed";
  std::size_t sent = 0, received = 0;
  int status = 0;
  bool reaped = false;
  bool entry_completed = false;
  while (result.error_code.empty()) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    if (cancelled && cancelled()) {
      result.error_code = "runner_cancelled";
      break;
    }
    if (elapsed >= static_cast<long long>(timeout_ms)) {
      result.error_code = "runner_timeout";
      break;
    }
    if (!reaped) {
      const auto waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        reaped = true;
        child.reaped = true;
      } else if (waited < 0 && errno != EINTR) {
        result.error_code = "runner_wait_failed";
        break;
      }
    }
    if (entry_status) {
      const auto state = entry_status();
      if (!state.empty() && state != "completed") {
        result.error_code = state;
        break;
      }
      if (reaped && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        result.error_code = "runner_nonzero_exit";
        break;
      }
      if (!state.empty()) {
        entry_completed = state == "completed";
        if (!entry_completed)
          result.error_code = state;
        break;
      }
    }
    if (fd[2] < 0 && fd[4] < 0) {
      // 派发进程可能先于异步工作进程退出。
      // 仍在原试验截止时间内监督整个进程组。
      if (!entry_status &&
          ((reaped && (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !capture_finished)) ||
           (capture_finished && capture_finished())))
        break;
    }
    if (fd[0] >= 0 && sent == input.size())
      fds.close_at(0);
    pollfd polling[3]{{fd[0], POLLOUT, 0}, {fd[2], POLLIN, 0}, {fd[4], POLLIN, 0}};
    if (::poll(polling, 3, 20) < 0 && errno != EINTR) {
      result.error_code = "runner_poll_failed";
      break;
    }
    if (fd[0] >= 0 && polling[0].revents) {
      const auto count = ::send(fd[0], input.data() + sent, input.size() - sent, MSG_NOSIGNAL);
      if (count > 0)
        sent += static_cast<std::size_t>(count);
      else if (count < 0 && errno != EINTR && errno != EAGAIN)
        fds.close_at(0);
    }
    for (int i = 1; i < 3; ++i) {
      const int index = i * 2;
      if (fd[index] < 0 || !polling[i].revents)
        continue;
      char buffer[8192];
      const auto count = ::read(fd[index], buffer, sizeof(buffer));
      if (count > 0) {
        received += static_cast<std::size_t>(count);
        if (received > max_output_bytes) {
          result.error_code = "runner_output_limit";
          break;
        }
        if (i == 1)
          result.output.append(buffer, count);
        // 持续排空标准错误流，但不保留正文，因为它可能包含凭据。
      } else if (count == 0)
        fds.close_at(index);
      else if (errno != EINTR && errno != EAGAIN) {
        result.error_code = "runner_read_failed";
        break;
      }
    }
  }
  // 私有子进程回收器在清理文件前等待脱离的后代进程退出。
  if (!child.stop(status))
    result.error_code = "runner_cleanup_timeout";
  if (entry_completed)
    result.exit_code = 0;
  else if (WIFEXITED(status))
    result.exit_code = WEXITSTATUS(status);
  if (result.error_code.empty() && result.exit_code != 0)
    result.error_code = "runner_nonzero_exit";
  result.duration_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  if (!result.error_code.empty())
    result.output.clear();
  return result;
}

} // 命名空间结束：trajectory::evolution
