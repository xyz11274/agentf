// 后台评测进程监督：保持作业与命令行会话解耦，记录执行状态。
// 取消、超时和异常退出要显式收尾；不得把进程存在当成评测已经成功。

#include "evaluation_process.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace trajectory::evolution {
namespace {
volatile std::sig_atomic_t stopping = 0;

// 记录停止信号，由监督主循环回收子进程；信号处理不执行复杂业务逻辑。
void stop_supervisor(int) {
  stopping = 1;
}
}

// 只在包的私有子进程运行，不能在 SDK 宿主进程设置此行为。
// 子进程回收器接管双重派生或脱离会话的工作进程，而不修改 Agent。
int run_evaluation_supervisor(int argc, char **argv) {
  if (argc < 4)
    return 125;
  char *end = nullptr;
  const auto parent = std::strtol(argv[2], &end, 10);
  if (!end || *end || parent <= 0 || ::getppid() != parent)
    return 125;

  struct sigaction action {};

  action.sa_handler = stop_supervisor;
  ::sigemptyset(&action.sa_mask);
  if (::sigaction(SIGTERM, &action, nullptr) || ::sigaction(SIGINT, &action, nullptr))
    return 125;
  sigset_t mask;
  if (::sigprocmask(SIG_SETMASK, nullptr, &mask))
    return 125;
  ::sigdelset(&mask, SIGTERM);
  ::sigdelset(&mask, SIGINT);
  if (::sigprocmask(SIG_SETMASK, &mask, nullptr) || ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) ||
      ::prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) || ::getppid() != parent)
    return 125;
  // 父进程在释放私有启动门之前设置资源限制。
  char ready = 0;
  ssize_t count;
  do {
    count = ::read(3, &ready, 1);
  } while (count < 0 && errno == EINTR && !stopping);
  ::close(3);
  if (count != 1 || ready != 'G' || stopping)
    return 125;
  const auto root = ::fork();
  if (root < 0)
    return 125;
  if (root == 0) {
    ::signal(SIGTERM, SIG_DFL);
    ::signal(SIGINT, SIG_DFL);
    ::execvp(argv[3], argv + 3);
    ::_exit(127);
  }
  ::close(STDIN_FILENO);
  int root_status = 125;
  for (;;) {
    int status = 0;
    pid_t child;
    while ((child = ::waitpid(-1, &status, WNOHANG)) > 0) {
      if (child != root)
        continue;
      root_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      if (root_status != 0)
        stopping = 1;
      else {
        // 此处读到流结束只表示派发进程退出，评测器仍可等待
        // 独立运行的工作进程通过采集终态结束任务。
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
      }
    }
    if (child < 0 && errno == ECHILD)
      return root_status;
    if (child < 0 && errno != EINTR)
      return 125;
    if (stopping) {
      // 这些都是尚未回收的直接子进程，因此其进程号不会被系统复用。
      // 终止父进程后接管其后代，重复回收直到没有子进程。
      std::ifstream children("/proc/self/task/" + std::to_string(::getpid()) + "/children");
      pid_t owned;
      while (children >> owned)
        if (owned > 0)
          ::kill(owned, SIGKILL);
    }
    ::poll(nullptr, 0, 10);
  }
}
} // 命名空间结束：trajectory::evolution
