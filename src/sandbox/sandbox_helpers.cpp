// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxHelpers 实现——工具类 helper（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T042：为每个工具构造 tool_params JSON，统一调 Sandbox::exec

#include "sandbox/sandbox_helpers.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <rapidjson/document.h>

namespace agentflow {
namespace sandbox {

namespace fs = std::filesystem;

// 简化 JSON 字符串转义（生产实现可用 rapidjson，此处为 MVP 范围）
static std::string escape_json(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

ExecResult SandboxHelpers::dispatch(Sandbox& sandbox, const std::string& tool,
                                    const std::string& tool_params_json, const ExecOptions& opts) {
  // 前置：sandbox.state() == RUNNING（由 Sandbox::exec 内部校验）
  return sandbox.exec(tool, tool_params_json, opts);
}

ExecResult SandboxHelpers::bash(Sandbox& sandbox, const std::string& command, const ExecOptions& opts) {
  std::string params = "{\"command\":\"" + escape_json(command) + "\"}";
  return dispatch(sandbox, "bash", params, opts);
}

// 其余工具方法（read_file/write_file/edit_file/list_skills/load_skill/grep/glob）
// 已迁移到 Python 侧 skill_executor → HTTPExecutor → POST /exec 路径，
// C++ SandboxHelpers 不再提供，详见 contracts/sandbox-http-server.md。

std::string SandboxHelpers::validate_upload_path(const std::string& path) {
  // 接受格式：workspace/{user_id}_{client_id}/upload/{filename}
  // 1) 必须以 workspace/ 开头
  if (path.find("workspace/") != 0) {
    return "PATH_NOT_IN_UPLOAD_DIR: path must start with "
           "'workspace/{session}/upload/', got: " + path;
  }
  // 2) 去掉 workspace/ 前缀后，剩余部分: {session}/upload/{filename}
  std::string rest = path.substr(strlen("workspace/"));
  auto upload_pos = rest.find("/upload/");
  if (upload_pos == std::string::npos || upload_pos == 0) {
    return "PATH_NOT_IN_UPLOAD_DIR: path must contain a non-empty session "
           "directory before '/upload/', got: " + path;
  }
  return "";  // 合法
}

std::string SandboxHelpers::rewrite_upload_path_to_sandbox(
    const std::string& content,
    const std::string& session_marker,
    const std::string& ws_prefix) {
  // ws_prefix 为空（Local/Docker 共享 workdir）→ 不改写，原样返回
  if (ws_prefix.empty()) return content;
  if (session_marker.empty()) return content;

  std::string abs_marker = ws_prefix + "/" + session_marker;
  // 幂等保护：abs_marker 以 session_marker 结尾（{ws_prefix}/{session_marker}），
  // 已改写的路径（{ws_prefix}/{session_marker}...）再次匹配 session_marker 时
  // 必须跳过，否则会二次改写产生 {ws_prefix}//{ws_prefix}/... 双斜线
  // （2026-08-12 修复：RewriteUploadPath_AlreadySandboxAbsNoDoubleRewrite 实测复现）。
  std::string ws_prefix_slash = ws_prefix + "/";
  std::string result = content;
  size_t pos = 0;
  while ((pos = result.find(session_marker, pos)) != std::string::npos) {
    // 匹配点前正好是 {ws_prefix}/ → 已处于沙箱绝对路径，跳过本次匹配
    if (pos >= ws_prefix_slash.size() &&
        result.compare(pos - ws_prefix_slash.size(), ws_prefix_slash.size(),
                       ws_prefix_slash) == 0) {
      pos += session_marker.size();
      continue;
    }
    result.replace(pos, session_marker.size(), abs_marker);
    pos += abs_marker.size();
  }
  return result;
}

ExecResult SandboxHelpers::parse_exec_result_json(const std::string& resp, const std::string& endpoint) {
  ExecResult result;
  result.exit_code = 1;
  result.stdout = "";
  result.stderr = "";
  result.truncated = false;
  result.duration_ms = 0;

  if (resp.empty()) {
    result.stderr = "Sandbox endpoint unreachable: " + endpoint;
    return result;
  }

  // 用 rapidjson 解析 JSON（对齐 ExecResult 结构化契约：sandbox_types.h + contracts/sandbox-http-server.md）
  // 替代手动字符串搜索——旧版依赖字符串长度推算（pos+12/pos+10 等魔数），改字段名就要改 5 处
  rapidjson::Document doc;
  doc.Parse(resp.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    result.stderr = resp;  // 无法解析，返回原始响应
    return result;
  }

  if (doc.HasMember("exit_code") && doc["exit_code"].IsInt()) {
    result.exit_code = doc["exit_code"].GetInt();
  } else {
    result.stderr = resp;  // 缺少 exit_code 字段，无法确认执行结果
    return result;
  }

  if (doc.HasMember("stdout") && doc["stdout"].IsString()) {
    result.stdout = doc["stdout"].GetString();
  }
  if (doc.HasMember("stderr") && doc["stderr"].IsString()) {
    result.stderr = doc["stderr"].GetString();
  }
  if (doc.HasMember("truncated") && doc["truncated"].IsBool()) {
    result.truncated = doc["truncated"].GetBool();
  }
  if (doc.HasMember("duration_ms") && doc["duration_ms"].IsInt64()) {
    result.duration_ms = doc["duration_ms"].GetInt64();
  }

  return result;
}

// ── 子进程执行（F5 单一真相来源：E2BSandbox / TemplateRegistry / e2b 子进程调用共用）──
// 原实现位于 sandbox_e2b.cpp（2026-08-08 提取），迁移后 sandbox_e2b.cpp 改用 SandboxHelpers::

const std::string& SandboxHelpers::resolve_python3() {
  static std::string cached;
  if (!cached.empty()) return cached;
  // 命1：PATH which python3
  FILE* fp = popen("which python3 2>/dev/null", "r");
  if (fp) {
    char buf[4096];
    if (fgets(buf, sizeof(buf), fp)) {
      std::string p = buf;
      while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
      if (!p.empty() && fs::exists(p)) cached = p;
    }
    pclose(fp);
  }
  // 命2：常见绝对路径候选（macOS Homebrew / Linux / conda）
  if (cached.empty()) {
    static const char* candidates[] = {
        "/opt/homebrew/bin/python3", "/usr/local/bin/python3",
        "/usr/bin/python3", "/bin/python3",
    };
    for (auto c : candidates) {
      if (fs::exists(c)) { cached = c; break; }
    }
  }
  // 命3：兜底——裸 "python3"（保留 PATH 依赖，至少 exit_code=127 能被 exec_cmd_argv 捕到）
  if (cached.empty()) cached = "python3";
  return cached;
}

SubprocessResult SandboxHelpers::exec_cmd_argv(const std::vector<std::string>& argv_strs,
                                               int timeout_ms) {
  int stdout_pipe[2];
  if (pipe(stdout_pipe) < 0) return {"", -1};

  pid_t pid = fork();
  if (pid < 0) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return {"", -1};
  }

  if (pid == 0) {
    // ── 子进程 ──
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stdout_pipe[1], STDERR_FILENO);  // stderr 合并到 stdout
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    // 构造 argv（execvp 要求 nullptr 结尾）
    std::vector<char*> argv;
    argv.reserve(argv_strs.size() + 1);
    // 注意：这里用 vector<char*> 指向 argv_strs 中各 string 的 c_str()。
    // argv_strs 在本函数栈上，子进程 execvp 前一直有效，安全。
    for (const auto& s : argv_strs) {
      argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    execvp(argv_strs[0].c_str(), argv.data());
    _exit(127);  // execvp 仅在失败时返回（如 python3 不在 PATH）
  }

  // ── 父进程 ──
  close(stdout_pipe[1]);

  std::string result;
  char buf[4096];
  ssize_t n;
  if (timeout_ms > 0) {
    // 超时执行：poll 等待子进程退出（读管道 EOF 或 WNOHANG 见退出），超时 kill。
    // 防止挂起的子进程（如 E2B snapshot 重建遇网络不可达）无限阻塞父进程。
    // 必须先置管道读端非阻塞：read() 默认阻塞，子进程无输出时会无限卡在 read 上，
    // remaining 递减与超时 kill 永远不可达（实测 180s 超时失效，daemon 卡死 9+ 分钟）。
    int pipe_flags = fcntl(stdout_pipe[0], F_GETFL, 0);
    if (pipe_flags != -1) {
      fcntl(stdout_pipe[0], F_SETFL, pipe_flags | O_NONBLOCK);
    }
    int remaining = timeout_ms;
    while (remaining > 0) {
      // 非阻塞读一次（已有数据则读走；无数据返回 EAGAIN，不阻塞）
      while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
        result.append(buf, n);
      }
      int status = 0;
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        // 子进程已退出：把剩余管道数据读完
        while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
          result.append(buf, n);
        }
        close(stdout_pipe[0]);
        int code = -1;
        if (WIFEXITED(status)) {
          code = WEXITSTATUS(status);  // 127 = execvp 失败；其他 = 脚本 exit code
        } else if (WIFSIGNALED(status)) {
          code = 128 + WTERMSIG(status);  // 被信号杀，约定 128+signum（与 shell 一致）
        }
        return {result, code};
      }
      // 未退出：sleep 一小段后继续轮询，直到超时
      usleep(100 * 1000);  // 100ms
      remaining -= 100;
    }
    // 超时：kill 子进程，返回 124（与 timeout 命令退出码约定一致）
    kill(pid, SIGKILL);
    // 恢复阻塞读：等子进程退出后读尽残留输出（EOF 才返回 0），
    // 避免非阻塞模式下 EAGAIN 提前退出循环导致尾部输出丢失
    if (pipe_flags != -1) {
      fcntl(stdout_pipe[0], F_SETFL, pipe_flags);
    }
    while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
      result.append(buf, n);
    }
    close(stdout_pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    // 输出保留（诊断用），退出码 124 表示超时
    (void)status;
    return {result, 124};
  }

  // ── 不限时（原行为）──
  while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
    result.append(buf, n);
  }
  close(stdout_pipe[0]);

  // 等待子进程退出（避免僵尸）+ 读 exit code（区分 execvp 失败 vs 脚本正常退出）
  int status;
  waitpid(pid, &status, 0);
  int code = -1;
  if (WIFEXITED(status)) {
    code = WEXITSTATUS(status);  // 127 = execvp 失败；其他 = 脚本 exit code
  } else if (WIFSIGNALED(status)) {
    code = 128 + WTERMSIG(status);  // 被信号杀，约定 128+signum（与 shell 一致）
  }

  return {result, code};
}

}  // namespace sandbox
}  // namespace agentflow
