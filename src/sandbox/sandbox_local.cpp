// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// LocalSandbox 实现——进程级沙箱（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T022：subprocess.Popen 启 sandbox_http_server.py，popen(curl) 调 POST /exec

#include "sandbox/sandbox_local.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <mutex>
#include <random>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "sandbox/sandbox_factory.h"  // SandboxLaunchException
#include "sandbox/sandbox_helpers.h"  // parse_exec_result_json

namespace agentflow {
namespace sandbox {

namespace fs = std::filesystem;

// 生成 UUID 风格的 sandbox_id（简化版：时间戳 + 随机数）
static std::string generate_sandbox_id() {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device rd;
  std::stringstream ss;
  ss << std::hex << now << "-" << rd() << "-" << rd();
  return ss.str();
}

// LocalSandbox 共用宿主网络命名空间。这里只串行化很短的 server 启动窗口，
// 避免后一实例在前一子进程完成 bind 前选中同一端口。
static std::mutex& local_server_launch_mutex() {
  static std::mutex mutex;
  return mutex;
}

// fork 前保留实际监听 socket，交给子进程直接使用；不能先 close 再 bind，
// 否则不同 daemon 的进程内锁无法阻止同时选中同一端口。
static int select_local_server_port(int requested_port, int* listener_fd) {
  auto try_bind = [listener_fd](int candidate, int* selected_port) -> int {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
      return errno;
    }
    if (fcntl(sock, F_SETFD, FD_CLOEXEC) < 0) {
      int error = errno;
      close(sock);
      return error;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(candidate));

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
      int error = errno;
      close(sock);
      return error;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
      int error = errno;
      close(sock);
      return error;
    }

    *selected_port = ntohs(addr.sin_port);
    *listener_fd = sock;
    return 0;
  };

  int selected_port = 0;
  int error = try_bind(requested_port > 0 ? requested_port : 0, &selected_port);
  if (error == EADDRINUSE && requested_port > 0) {
    error = try_bind(0, &selected_port);
  }
  if (error != 0 || selected_port <= 0) {
    throw SandboxFactory::SandboxLaunchException(
        "Failed to select LocalSandbox HTTP port: " + std::string(strerror(error)));
  }
  return selected_port;
}

LocalSandbox::LocalSandbox(const SandboxKey& owner,
                           const std::string& workdir_root,
                           const std::string& sandbox_http_server_path,
                           int sandbox_http_port,
                           int64_t output_max_bytes)
    : owner_(owner),
      sandbox_id_(generate_sandbox_id()),
      workdir_(workdir_root + "/" + sandbox_id_),
      state_(SandboxState::RUNNING),
      output_max_bytes_(output_max_bytes),
      server_pid_(-1),
      http_server_path_(sandbox_http_server_path),
      http_server_port_(sandbox_http_port) {
  // 创建 workdir（对齐 B6：按 sandbox_id 隔离工作区）
  std::error_code ec;
  fs::create_directories(workdir_, ec);
  if (ec) {
    throw SandboxFactory::SandboxLaunchException(
        "Failed to create workdir: " + workdir_ + " (" + ec.message() + ")");
  }

  // 拉起 sandbox_http_server.py（端口冲突时自动分配独立端口）
  int port = launch_http_server(sandbox_http_server_path, sandbox_http_port);
  endpoint_ = "http://127.0.0.1:" + std::to_string(port);  // B17：只监听 loopback

  // endpoint 不可达时 wait_for_server_ready 已抛异常——此处 state_ 已是 RUNNING
  mark_active();  // FR-009：初始化活跃时间戳
}

LocalSandbox::~LocalSandbox() {
  if (state_ == SandboxState::RUNNING) {
    destroy();
  }
}

int LocalSandbox::launch_http_server(const std::string& sandbox_http_server_path, int port) {
  // 用 fork+execl 启 python3 sandbox_http_server.py（popen 的非阻塞版本）
  // 与 exe_loader.cpp 既有 fork+exec 模式一致（F5 单一真相来源）

  std::lock_guard<std::mutex> launch_guard(local_server_launch_mutex());
  int listener_fd = -1;
  port = select_local_server_port(port, &listener_fd);
  const std::string listener_fd_str = std::to_string(listener_fd);

  // FR-024：在 fork 前计算父进程 PID，避免 fork 后子进程 getppid() 竞态
  // （fork 后、getppid() 前父进程死亡 → 子进程被 init 收养 → getppid()==1，
  //  此时 --parent-pid 1 会让 _parent_monitor 永远检测不退出，孤儿进程残留）
  // 在父进程中 getpid() 即本进程 PID，子进程 fork 后继承变量值，无竞态
  const std::string parent_pid_str = std::to_string(getpid());

  pid_t pid = fork();
  if (pid < 0) {
    close(listener_fd);
    throw SandboxFactory::SandboxLaunchException("fork() failed");
  }
  if (pid == 0) {
    // 仅本次 server 的 exec 保留 socket，其他 worker 不继承。
    if (fcntl(listener_fd, F_SETFD, 0) < 0) _exit(127);
    // 子进程：重定向 stderr→stdout（供父进程读端口），chdir 到 workdir
    dup2(STDOUT_FILENO, STDERR_FILENO);
    (void)chdir(workdir_.c_str());

    // FR-024：父进程异常死亡检测
    // 1. Linux：PR_SET_PDEATHSIG 内核级防护（父进程死 → 子进程收到 SIGTERM）
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    // 2. --parent-pid 参数（macOS 兜底）：fork 前已计算，子进程直接继承
    //    注意：必须先 prctl 再 execlp，避免 prctl 前父进程死亡导致竞态
    //    （此时 getppid()==1，_parent_monitor 线程会检测到并退出）

    // execl python3 sandbox_http_server.py --port <port> --host 127.0.0.1 --parent-pid <ppid>
    execlp("python3", "python3", sandbox_http_server_path.c_str(),
           "--port", std::to_string(port).c_str(),
           "--listen-fd", listener_fd_str.c_str(),
           "--host", "127.0.0.1",  // B17：只监听 loopback
           "--parent-pid", parent_pid_str.c_str(),
           "--security-profile", "restricted-local",
           nullptr);
    // execlp 仅在失败时返回
    _exit(127);
  }
  // 父进程
  close(listener_fd);
  server_pid_ = pid;

  // 持有启动锁直到当前子进程真正占有选定端口。否则后一实例可能探测到旧
  // server 的响应，并把兄弟实例的 endpoint 误认成自己的 endpoint。
  try {
    wait_for_server_ready(port);
  } catch (...) {
    // 构造失败不会调用析构，必须回收尚未就绪的子进程及其监听 socket。
    if (server_pid_ > 0) {
      kill(server_pid_, SIGKILL);
      while (waitpid(server_pid_, nullptr, 0) < 0 && errno == EINTR) {}
      server_pid_ = -1;
    }
    throw;
  }
  return port;
}

void LocalSandbox::wait_for_server_ready(int port, int timeout_ms) {
  // 轮询 curl endpoint 直到成功或超时
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string probe = "http://127.0.0.1:" + std::to_string(port) + "/exec";
  while (std::chrono::steady_clock::now() < deadline) {
    // 用 popen(curl) 探测——发一个空 POST 看是否响应
    std::string cmd = "curl -s --noproxy '*' -m 1 -X POST -H 'Content-Type: application/json' "
                      "-d '{\"tool\":\"bash\",\"tool_params\":{\"command\":\"echo ready\"},\"opts\":{}}' "
                      "'" + probe + "' 2>/dev/null";
    bool ready = false;
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
      char buf[256];
      std::string resp;
      while (fgets(buf, sizeof(buf), fp)) resp += buf;
      pclose(fp);
      ready = !resp.empty();
    }
    // 检查子进程是否已退出（启动失败）
    int status;
    pid_t waited = waitpid(server_pid_, &status, WNOHANG);
    if (waited == server_pid_) {
      server_pid_ = -1;
      throw SandboxFactory::SandboxLaunchException(
          "sandbox_http_server.py exited prematurely (exit status=" + std::to_string(status) + ")");
    }
    if (ready && waited == 0) return;
    usleep(100000);  // 100ms 轮询间隔
  }
  // 超时
  throw SandboxFactory::SandboxLaunchException("sandbox_http_server.py startup timeout after "
                                               + std::to_string(timeout_ms) + "ms");
}

std::string LocalSandbox::curl_post(const std::string& path, const std::string& body_json, int64_t timeout_ms) {
  // 安全版本：fork+execvp 数组形式 + stdin 管道传 body（避免 shell 注入，CWE-77）
  // body 通过 --data-binary @- 从 stdin 读取，不经过命令行参数
  // 与 exe_loader.cpp 既有 fork+exec 模式一致（F5 单一真相来源）
  // 认证头由可信 HTTP server 写入私有文件，值不进入命令行或工具环境。
  const std::string auth_header = "@/tmp/agentflow-local-auth-" + std::to_string(getuid())
                                  + "/" + endpoint_.substr(endpoint_.rfind(':') + 1);
  int stdin_pipe[2];
  int stdout_pipe[2];

  if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
    return "";  // pipe 失败，调用方按空响应处理错误
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdin_pipe[0]); close(stdin_pipe[1]);
    close(stdout_pipe[0]); close(stdout_pipe[1]);
    return "";
  }

  if (pid == 0) {
    // 子进程：重定向 stdin → pipe 读端，stdout → pipe 写端
    close(stdin_pipe[1]);   // 关闭写端（子进程只读）
    dup2(stdin_pipe[0], STDIN_FILENO);
    close(stdin_pipe[0]);

    close(stdout_pipe[0]);  // 关闭读端（子进程只写）
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdout_pipe[1]);

    // stderr 重定向到 /dev/null（与旧版 2>/dev/null 等效）
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    int timeout_s = static_cast<int>((timeout_ms + 999) / 1000);
    char timeout_str[16];
    snprintf(timeout_str, sizeof(timeout_str), "%d", timeout_s + 5);

    // execvp 数组形式，不经过 shell，无注入风险
    char* const args[] = {
        const_cast<char*>("curl"),
        const_cast<char*>("-s"),
        const_cast<char*>("--noproxy"), const_cast<char*>("*"),
        const_cast<char*>("-m"), timeout_str,
        const_cast<char*>("-X"), const_cast<char*>("POST"),
        const_cast<char*>("-H"), const_cast<char*>("Content-Type: application/json"),
        const_cast<char*>("-H"), const_cast<char*>(auth_header.c_str()),
        const_cast<char*>("--data-binary"), const_cast<char*>("@-"),
        const_cast<char*>(path.c_str()),
        nullptr
    };
    execvp("curl", args);
    _exit(127);  // execvp 仅在失败时返回
  }

  // 父进程：关闭读端（只写 stdin），关闭写端（只读 stdout）
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);

  // 向子进程 stdin 写入 body（通过 --data-binary @- 读取）
  const char* body = body_json.data();
  size_t body_len = body_json.size();
  ssize_t written = 0;
  while (written < static_cast<ssize_t>(body_len)) {
    ssize_t n = write(stdin_pipe[1], body + written, body_len - written);
    if (n <= 0) break;
    written += n;
  }
  close(stdin_pipe[1]);  // 关闭写端，通知子进程 EOF

  // 读子进程 stdout
  std::string resp;
  char buf[4096];
  ssize_t n;
  while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
    resp.append(buf, n);
  }
  close(stdout_pipe[0]);

  // 等待子进程退出
  int status;
  waitpid(pid, &status, 0);

  return resp;
}

ExecResult LocalSandbox::exec(const std::string& tool,
                              const std::string& tool_params_json,
                              const ExecOptions& opts) {
  if (state_ != SandboxState::RUNNING) {
    ExecResult r;
    r.exit_code = 1;
    r.stderr = "Sandbox not RUNNING (state=" + std::to_string(static_cast<int>(state_)) + ")";
    return r;
  }

  // 构造 POST /exec 请求体（对齐 contracts/sandbox-http-server.md）
  // tool_params_json 已是 JSON 字符串，需嵌入到外层请求 JSON
  int64_t timeout_ms = opts.timeout_ms > 0 ? opts.timeout_ms : 30000;  // 默认 30s
  std::string working_dir = opts.working_dir.empty() ? workdir_ : opts.working_dir;

  // env_vars 转为 JSON object（对齐 contracts/sandbox-http-server.md opts.env_vars）
  // 手动转义 value 中的 " 和 \ 避免 JSON 格式错误（CWE-117）
  std::string env_json = "{";
  bool first = true;
  for (auto it = opts.env_vars.begin(); it != opts.env_vars.end(); ++it) {
    if (!first) env_json += ",";
    // 转义 key 和 value 中的引号和反斜杠
    std::string safe_k = it->first, safe_v = it->second;
    for (auto& c : safe_k) { if (c == '"' || c == '\\') c = '_'; }
    for (auto& c : safe_v) { if (c == '"' || c == '\\') c = '_'; }
    env_json += "\"" + safe_k + "\":\"" + safe_v + "\"";
    first = false;
  }
  env_json += "}";

  std::string body = "{\"tool\":\"" + tool + "\","
                     "\"tool_params\":" + tool_params_json + ","
                     "\"opts\":{\"timeout_ms\":" + std::to_string(timeout_ms) + ","
                     "\"working_dir\":\"" + working_dir + "\","
                     "\"env_vars\":" + env_json + ","
                     "\"output_max_bytes\":" + std::to_string(output_max_bytes_) + "}}";

  std::string path = endpoint_ + "/exec";
  std::string resp = curl_post(path, body, timeout_ms);

  mark_active();  // FR-009：每次 exec 更新活跃时间
  return SandboxHelpers::parse_exec_result_json(resp, endpoint_);
}

void LocalSandbox::destroy() {
  // B7：幂等——多次调用一致，进程已死时无操作
  if (state_ == SandboxState::RECYCLING) {
    return;  // 已销毁，幂等
  }
  state_ = SandboxState::RECYCLING;  // B21：RECYCLING 终态不可逆

  if (server_pid_ > 0) {
    // terminate 子进程：先 SIGTERM，等待 1s，再 SIGKILL
    kill(server_pid_, SIGTERM);
    int status;
    pid_t waited = waitpid(server_pid_, &status, WNOHANG);
    if (waited == 0) {
      // 1s 内未退出，SIGKILL
      usleep(1000000);
      kill(server_pid_, SIGKILL);
      waitpid(server_pid_, &status, 0);
    }
    server_pid_ = -1;
  }
  // workdir 不清理——快照兜底（B11/I-1），由 SandboxSnapshots 负责
}

void LocalSandbox::pause() {
  // B21：RUNNING→STOPPED 可逆（暂停/恢复）；B7 幂等
  if (state_ == SandboxState::STOPPED || state_ == SandboxState::RECYCLING) {
    return;  // 已暂停/已销毁，无操作
  }
  state_ = SandboxState::STOPPED;

  if (server_pid_ > 0) {
    // 与 destroy() 相同的 terminate 策略：SIGTERM → 等 1s → SIGKILL
    kill(server_pid_, SIGTERM);
    int status;
    pid_t waited = waitpid(server_pid_, &status, WNOHANG);
    if (waited == 0) {
      usleep(1000000);
      kill(server_pid_, SIGKILL);
      waitpid(server_pid_, &status, 0);
    }
    server_pid_ = -1;
  }
  // workdir 保留（B21 可逆，resume 后恢复同一 workdir）
  mark_active();  // FR-009：暂停也是活动（避免 GC 立即回收）
}

void LocalSandbox::resume() {
  // B21：STOPPED→RUNNING 可逆；B7 幂等
  if (state_ != SandboxState::STOPPED) {
    return;  // 非暂停态无操作（RUNNING 或 RECYCLING）
  }

  // 重新拉起 sandbox_http_server.py（端口复用 http_server_port_，0=自动分配）
  int port = launch_http_server(http_server_path_, http_server_port_);
  endpoint_ = "http://127.0.0.1:" + std::to_string(port);  // B17：只监听 loopback

  state_ = SandboxState::RUNNING;
  mark_active();  // FR-009：恢复后重置活跃时间戳
}

}  // namespace sandbox
}  // namespace agentflow
