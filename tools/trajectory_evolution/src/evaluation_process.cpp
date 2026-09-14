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
  void close_at(int index) {
    if(value[index] >= 0) { ::close(value[index]); value[index] = -1; }
  }
  ~Descriptors() { for(int i = 0; i < 8; ++i) close_at(i); }
};
struct Child {
  pid_t pid;
  bool reaped = false;
  bool stop(int& status) {
    if(pid <= 0 || reaped) return true;
    ::kill(pid, SIGTERM);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    do {
      const auto waited = ::waitpid(pid, &status, WNOHANG);
      if(waited == pid) { reaped = true; return true; }
      if(waited < 0 && errno != EINTR) { reaped = true; return false; }
      ::poll(nullptr, 0, 10);
    } while(Clock::now() < deadline);
    ::kill(-pid, SIGKILL);
    while(::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    reaped = true;
    return false;
  }
  ~Child() {
    int status = 0;
    stop(status);
  }
};

std::string supervisor_executable() {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidates;
  std::error_code error;
  const auto executable = fs::read_symlink("/proc/self/exe", error);
  if(!error) candidates.push_back(executable.parent_path() / "trajectory_evolution");
  static int module_anchor;
  Dl_info module{};
  if(::dladdr(&module_anchor, &module) && module.dli_fname) {
    const auto directory = fs::absolute(module.dli_fname).parent_path();
    candidates.push_back(directory / "trajectory_evolution");
    candidates.push_back(directory.parent_path() / "bin/trajectory_evolution");
  }
  candidates.emplace_back(EVOLUTION_INSTALLED_CLI);
  candidates.emplace_back(EVOLUTION_BUILD_CLI);
  for(const auto& candidate : candidates) {
    error.clear();
    if(fs::is_regular_file(candidate, error) && !error && ::access(candidate.c_str(), X_OK) == 0)
      return candidate.string();
  }
  return {};
}
}

EvaluationProcessResult run_evaluation_process(
    const std::vector<std::string>& command,
    const std::vector<std::string>& environment,
    const std::filesystem::path& directory, const std::string& input,
    std::size_t timeout_ms, std::size_t max_output_bytes,
    const std::function<bool()>& cancelled, const EvaluationProcessLimits& limits,
    const std::function<bool()>& capture_finished,
    const std::function<std::string()>& entry_status) {
  EvaluationProcessResult result;
  const auto started = Clock::now();
  const auto supervisor = supervisor_executable();
  if(supervisor.empty()) { result.error_code = "runner_supervisor_not_found"; return result; }
  Descriptors fds;
  auto& fd = fds.value;
  if(command.empty() || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd.data())
      || ::pipe2(fd.data() + 2, O_CLOEXEC) || ::pipe2(fd.data() + 4, O_CLOEXEC)
      || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd.data() + 6)) {
    result.error_code = "runner_pipe_failed"; return result;
  }
  std::vector<char*> argv, envp;
  std::vector<std::string> supervised{supervisor, "--internal-evaluation-supervisor", std::to_string(::getpid())};
  supervised.insert(supervised.end(), command.begin(), command.end());
  for(const auto& item : supervised) argv.push_back(const_cast<char*>(item.c_str()));
  for(const auto& item : environment) envp.push_back(const_cast<char*>(item.c_str()));
  argv.push_back(nullptr); envp.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  if(posix_spawn_file_actions_init(&actions)) {
    result.error_code = "runner_spawn_failed"; return result;
  }
  if(posix_spawnattr_init(&attributes)) {
    posix_spawn_file_actions_destroy(&actions);
    result.error_code = "runner_spawn_failed"; return result;
  }
  int error = posix_spawn_file_actions_adddup2(&actions, fd[1], STDIN_FILENO);
  error |= posix_spawn_file_actions_adddup2(&actions, fd[3], STDOUT_FILENO);
  error |= posix_spawn_file_actions_adddup2(&actions, fd[5], STDERR_FILENO);
  // FD 3 is a private startup gate; the supervisor closes it before the Agent
  // starts. No service locks or other descriptors cross into the trial.
  error |= posix_spawn_file_actions_adddup2(&actions, fd[6], 3);
  error |= posix_spawn_file_actions_addclosefrom_np(&actions, 4);
  error |= posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  error |= posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  error |= posix_spawnattr_setpgroup(&attributes, 0);
  pid_t pid = -1;
  if(!error) error = posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if(error) { result.error_code = "runner_spawn_failed"; return result; }
  Child child{pid};
  // Apply limits before sending the task. Descendants inherit these limits;
  // deployment-level containers/cgroups may further bound the complete tree.
  bool limited = true;
  if(limits.nice) limited &= ::setpriority(PRIO_PROCESS, pid, limits.nice) == 0;
#if defined(__linux__)
  if(limits.address_space_bytes) {
    const rlimit bound{limits.address_space_bytes, limits.address_space_bytes};
    limited &= ::prlimit(pid, RLIMIT_AS, &bound, nullptr) == 0;
  }
  if(limits.cpu_seconds) {
    const rlimit bound{limits.cpu_seconds, limits.cpu_seconds};
    limited &= ::prlimit(pid, RLIMIT_CPU, &bound, nullptr) == 0;
  }
#else
  limited &= !limits.address_space_bytes && !limits.cpu_seconds;
#endif
  if(!limited) { result.error_code = "runner_resource_limits_failed"; return result; }
  if(::send(fd[7], "G", 1, MSG_NOSIGNAL) != 1) {
    result.error_code = "runner_supervisor_start_failed"; return result;
  }
  fds.close_at(6); fds.close_at(7);
  fds.close_at(1); fds.close_at(3); fds.close_at(5);
  for(int index : {0, 2, 4})
    if(fcntl(fd[index], F_SETFL, fcntl(fd[index], F_GETFL) | O_NONBLOCK) < 0)
      result.error_code = "runner_pipe_failed";
  std::size_t sent = 0, received = 0;
  int status = 0;
  bool reaped = false;
  bool entry_completed = false;
  while(result.error_code.empty()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    if(cancelled && cancelled()) { result.error_code = "runner_cancelled"; break; }
    if(elapsed >= static_cast<long long>(timeout_ms)) { result.error_code = "runner_timeout"; break; }
    if(!reaped) {
      const auto waited = ::waitpid(pid, &status, WNOHANG);
      if(waited == pid) { reaped = true; child.reaped = true; }
      else if(waited < 0 && errno != EINTR) { result.error_code = "runner_wait_failed"; break; }
    }
    if(entry_status) {
      const auto state = entry_status();
      if(!state.empty() && state != "completed") { result.error_code = state; break; }
      if(reaped && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        result.error_code = "runner_nonzero_exit"; break;
      }
      if(!state.empty()) {
        entry_completed = state == "completed";
        if(!entry_completed) result.error_code = state;
        break;
      }
    }
    if(fd[2] < 0 && fd[4] < 0) {
      // A dispatcher may exit before its asynchronous workers finish the run.
      // Keep supervising its process group under the original trial deadline.
      if(!entry_status && ((reaped && (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !capture_finished))
          || (capture_finished && capture_finished()))) break;
    }
    if(fd[0] >= 0 && sent == input.size()) fds.close_at(0);
    pollfd polling[3]{{fd[0], POLLOUT, 0}, {fd[2], POLLIN, 0}, {fd[4], POLLIN, 0}};
    if(::poll(polling, 3, 20) < 0 && errno != EINTR) {
      result.error_code = "runner_poll_failed"; break;
    }
    if(fd[0] >= 0 && polling[0].revents) {
      const auto count = ::send(fd[0], input.data() + sent, input.size() - sent, MSG_NOSIGNAL);
      if(count > 0) sent += static_cast<std::size_t>(count);
      else if(count < 0 && errno != EINTR && errno != EAGAIN) fds.close_at(0);
    }
    for(int i = 1; i < 3; ++i) {
      const int index = i * 2;
      if(fd[index] < 0 || !polling[i].revents) continue;
      char buffer[8192];
      const auto count = ::read(fd[index], buffer, sizeof(buffer));
      if(count > 0) {
        received += static_cast<std::size_t>(count);
        if(received > max_output_bytes) { result.error_code = "runner_output_limit"; break; }
        if(i == 1) result.output.append(buffer, count);
        // stderr is drained but never retained; it may contain credentials.
      } else if(count == 0) fds.close_at(index);
      else if(errno != EINTR && errno != EAGAIN) { result.error_code = "runner_read_failed"; break; }
    }
  }
  // The private subreaper drains detached descendants before files are removed.
  if(!child.stop(status)) result.error_code = "runner_cleanup_timeout";
  if(entry_completed) result.exit_code = 0;
  else if(WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
  if(result.error_code.empty() && result.exit_code != 0) result.error_code = "runner_nonzero_exit";
  result.duration_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  if(!result.error_code.empty()) result.output.clear();
  return result;
}

}  // namespace trajectory::evolution
