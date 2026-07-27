// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DockerSandbox 实现——容器级沙箱（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T073：docker run -p 0:8080 启动镜像 + 端口映射拿 endpoint

#include "sandbox/sandbox_docker.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "sandbox/sandbox_factory.h"  // SandboxLaunchException
#include "sandbox/sandbox_helpers.h"  // parse_exec_result_json

namespace agentflow {
namespace sandbox {

namespace fs = std::filesystem;

static std::string generate_sandbox_id() {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device rd;
  std::stringstream ss;
  ss << std::hex << now << "-" << rd() << "-" << rd();
  return ss.str();
}

// 执行 shell 命令并返回 stdout（简化 popen 封装）
static std::string exec_cmd(const std::string& cmd) {
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) return "";
  char buf[4096];
  std::string result;
  while (fgets(buf, sizeof(buf), fp)) result += buf;
  pclose(fp);
  return result;
}

DockerSandbox::DockerSandbox(const SandboxKey& owner,
                             const std::string& workdir_root,
                             const std::string& sandbox_http_server_path,
                             int sandbox_http_port,
                             int64_t output_max_bytes,
                             const std::string& skills_src_dir,
                             const std::string& image_name)
    : owner_(owner),
      sandbox_id_(generate_sandbox_id()),
      // workdir_root 可能为相对路径（如 "tmp/dev/sandbox"），Docker -v 要求绝对路径
      // 用 fs::absolute() 确保 Docker 不会因非法 volume name 拒绝
      workdir_(fs::absolute(workdir_root).string() + "/" + sandbox_id_),
      image_name_(image_name),
      skills_src_dir_(skills_src_dir),
      output_max_bytes_(output_max_bytes),
      state_(SandboxState::RUNNING) {
  // 创建 workdir
  std::error_code ec;
  fs::create_directories(workdir_, ec);
  if (ec) {
    throw SandboxFactory::SandboxLaunchException(
        "Failed to create workdir: " + workdir_ + " (" + ec.message() + ")");
  }

  // 宿主是模板，沙箱是瞬态副本：将 skills 复制到 workdir/harness/skills/
  // 不挂载额外 volume——构造时一次性复制到 workdir
  if (!skills_src_dir_.empty()) {
    fs::path skills_src = fs::absolute(skills_src_dir_);
    if (fs::is_directory(skills_src)) {
      fs::path skills_dst = fs::path(workdir_) / "harness" / "skills";
      fs::create_directories(skills_dst, ec);
      // 遍历源目录，复制每个 skill 子目录
      for (auto const& entry : fs::directory_iterator(skills_src)) {
        if (fs::is_directory(entry)) {
          fs::path target = skills_dst / entry.path().filename();
          fs::copy(entry.path(), target,
                   fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                   ec);
        }
      }
    }
  }

  // docker run 拉起容器
  auto run_result = run_container(sandbox_http_port);
  container_id_ = run_result.container_id;
  endpoint_ = "http://127.0.0.1:" + std::to_string(run_result.host_port);
  mark_active();  // FR-009：初始化活跃时间戳
}

DockerSandbox::~DockerSandbox() {
  if (state_ == SandboxState::RUNNING) {
    destroy();
  }
}

DockerSandbox::DockerRunResult DockerSandbox::run_container(int sandbox_http_port) {
  // docker run -d --rm --label agentflow.sandbox=1 --name <sandbox_id>
  //   -p 0:<port> -v <workdir>:/workdir -e DAEMON_PID=<pid> -e SANDBOX_WORKDIR=/workdir <image>
  // --rm: 容器停止时自动删除（FR-025 兜底）
  // --label agentflow.sandbox=1: 供启动时孤儿清理识别（FR-025 L3）
  // -e DAEMON_PID: 供容器内 sandbox_http_server.py 的 _parent_monitor 检测父进程死亡（FR-024）
  // skills 已由构造阶段复制到 workdir/harness/skills/，无需 SKILLS_SRC_DIR
  std::string cmd = "docker run -d --rm --label agentflow.sandbox=1 "
                    "--name 'sandbox-" + sandbox_id_ + "' "
                    "-p 0:" + std::to_string(sandbox_http_port) + " "
                    "-v '" + workdir_ + ":/workdir' "
                    "-e DAEMON_PID='" + std::to_string(getpid()) + "' "
                    "-e SANDBOX_WORKDIR='/workdir' ";
  cmd += "'" + image_name_ + "' 2>&1";
  std::string output = exec_cmd(cmd);

  if (output.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "Docker run failed: no output from docker command");
  }

  // 检查 docker 错误
  if (output.find("Error") != std::string::npos ||
      output.find("Cannot") != std::string::npos) {
    throw SandboxFactory::SandboxLaunchException(
        "Docker run failed: " + output);
  }

  // 解析 container_id（第一行，去除换行）
  std::string container_id = output;
  container_id.erase(container_id.find_last_not_of(" \n\r\t") + 1);
  if (container_id.size() < 12) {
    throw SandboxFactory::SandboxLaunchException(
        "Docker run: invalid container_id '" + container_id + "'");
  }

  // 查端口映射——docker port <container_id> 拿宿主机端口
  std::string port_cmd = "docker port '" + container_id + "' 2>/dev/null | head -1";
  std::string port_output = exec_cmd(port_cmd);
  int host_port = sandbox_http_port;
  if (!port_output.empty()) {
    // 输出格式: "8080/tcp -> 0.0.0.0:49153"
    auto colon = port_output.rfind(':');
    if (colon != std::string::npos) {
      std::string port_str = port_output.substr(colon + 1);
      host_port = std::stoi(port_str);
    }
  }

  return {container_id, host_port};
}

ExecResult DockerSandbox::exec(const std::string& tool,
                               const std::string& tool_params_json,
                               const ExecOptions& opts) {
  if (state_ != SandboxState::RUNNING) {
    ExecResult r;
    r.exit_code = 1;
    r.stderr = "Sandbox not RUNNING (state=" + std::to_string(static_cast<int>(state_)) + ")";
    return r;
  }

  // 同 LocalSandbox 的 curl 方案——HTTP POST 到 endpoint /exec
  int64_t timeout_ms = opts.timeout_ms > 0 ? opts.timeout_ms : 30000;
  int timeout_s = static_cast<int>((timeout_ms + 999) / 1000);
  std::string cmd = "curl -s -m " + std::to_string(timeout_s + 5)
                    + " -X POST -H 'Content-Type: application/json'"
                    + " -d '" + tool_params_json + "'"
                    + " '" + endpoint_ + "/exec' 2>/dev/null";
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) {
    ExecResult r;
    r.exit_code = 1;
    r.stderr = "Sandbox endpoint unreachable: " + endpoint_;
    return r;
  }
  char buf[4096];
  std::string resp;
  while (fgets(buf, sizeof(buf), fp)) resp += buf;
  pclose(fp);

  mark_active();  // FR-009：每次 exec 更新活跃时间
  return SandboxHelpers::parse_exec_result_json(resp, endpoint_);
}

void DockerSandbox::destroy() {
  // B7：幂等——多次调用一致，容器已停时无操作
  if (state_ == SandboxState::RECYCLING) return;
  state_ = SandboxState::RECYCLING;

  if (!container_id_.empty()) {
    // docker stop + docker rm（幂等）
    exec_cmd("docker stop 'sandbox-" + sandbox_id_ + "' 2>/dev/null");
    exec_cmd("docker rm -f 'sandbox-" + sandbox_id_ + "' 2>/dev/null");
    container_id_.clear();
  }
}

}  // namespace sandbox
}  // namespace agentflow