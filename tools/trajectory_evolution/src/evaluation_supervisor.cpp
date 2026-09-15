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
void stop_supervisor(int) { stopping = 1; }
}

// Runs only in the package's private subprocess, never in an SDK host. A
// subreaper owns even double-forked/setsid workers without changing the Agent.
int run_evaluation_supervisor(int argc, char** argv) {
  if(argc < 4) return 125;
  char* end = nullptr;
  const auto parent = std::strtol(argv[2], &end, 10);
  if(!end || *end || parent <= 0 || ::getppid() != parent) return 125;
  struct sigaction action{};
  action.sa_handler = stop_supervisor;
  ::sigemptyset(&action.sa_mask);
  if(::sigaction(SIGTERM, &action, nullptr) || ::sigaction(SIGINT, &action, nullptr)) return 125;
  sigset_t mask;
  if(::sigprocmask(SIG_SETMASK, nullptr, &mask)) return 125;
  ::sigdelset(&mask, SIGTERM); ::sigdelset(&mask, SIGINT);
  if(::sigprocmask(SIG_SETMASK, &mask, nullptr)
      || ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)
      || ::prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) || ::getppid() != parent) return 125;
  // The parent applies resource limits before releasing this private gate.
  char ready = 0;
  ssize_t count;
  do { count = ::read(3, &ready, 1); } while(count < 0 && errno == EINTR && !stopping);
  ::close(3);
  if(count != 1 || ready != 'G' || stopping) return 125;
  const auto root = ::fork();
  if(root < 0) return 125;
  if(root == 0) {
    ::signal(SIGTERM, SIG_DFL); ::signal(SIGINT, SIG_DFL);
    ::execvp(argv[3], argv + 3);
    ::_exit(127);
  }
  ::close(STDIN_FILENO);
  int root_status = 125;
  for(;;) {
    int status = 0;
    pid_t child;
    while((child = ::waitpid(-1, &status, WNOHANG)) > 0) {
      if(child != root) continue;
      root_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      if(root_status != 0) stopping = 1;
      else {
        // EOF now means the dispatcher exited. The evaluator can still wait
        // for independently running workers to close their captured runs.
        ::close(STDOUT_FILENO); ::close(STDERR_FILENO);
      }
    }
    if(child < 0 && errno == ECHILD) return root_status;
    if(child < 0 && errno != EINTR) return 125;
    if(stopping) {
      // These are our unreaped direct children, so their PIDs cannot be reused.
      // Killing a parent adopts its descendants here; repeat until ECHILD.
      std::ifstream children("/proc/self/task/" + std::to_string(::getpid()) + "/children");
      pid_t owned;
      while(children >> owned) if(owned > 0) ::kill(owned, SIGKILL);
    }
    ::poll(nullptr, 0, 10);
  }
}
}  // namespace trajectory::evolution
