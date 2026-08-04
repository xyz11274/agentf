// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// E2BSandbox 实现——远程沙箱（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T075：E2B SDK 拉起远程沙箱 + 公共 URL + token 鉴权
// 注意：E2B SDK 是 Python 包（e2b-code-interpreter），C++ 侧用 popen("python3 ...") 间接调用
// 生产实现应通过 Python 子进程节点调用 E2B SDK

#include "sandbox/sandbox_e2b.h"

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

#include <rapidjson/document.h>

// utils/logger.h 在 agentflow 库内部，非子进程脚本——用于 E2BSandbox 自身日志
#include "utils/logger.h"

namespace agentflow {
namespace sandbox {

namespace fs = std::filesystem;

// ── python3 绝对路径探测（lazy + 缓存）────────────────────────────────
// 真根因修复：execvp("python3") 在 daemon PATH 无 python3 时失败 _exit(127)，父进程
// 拿空串被上层只检 ERROR/FATAL/Traceback 的判定静默吞 → skills sync 没跑 → 沙箱里
// harness/skills 空。改 exec_cmd_argv 检 exit_code 是兜底，但根治 = 用绝对路径直调。
// 探测序：PATH which python3 → 常见绝对路径候选 → 裸 "python3" 兜底（保留 PATH 依赖）
static const std::string& resolve_python3() {
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

static std::string generate_sandbox_id() {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device rd;
  std::stringstream ss;
  ss << std::hex << now << "-" << rd() << "-" << rd();
  return ss.str();
}

static std::string exec_cmd(const std::string& cmd) {
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) return "";
  char buf[4096];
  std::string result;
  while (fgets(buf, sizeof(buf), fp)) result += buf;
  pclose(fp);
  return result;
}

// execvp 形式：参数以 argv 数组直传，不经 shell。
// 解决两个问题（CWE-78 shell 注入 + CWE-522 凭据暴露在 ps）：
//   1. traffic_access_token / sandbox_id / 路径 等含 ' 或 $ 时不会被 shell 解释
//   2. token 不出现在子进程的 ps cmdline 中
// 与 curl_post_with_token 的 fork+execvp 模式一致（F5 单一真相来源）。
// stderr 合并到 stdout（dup2(stdout_pipe[1], STDERR_FILENO)），便于诊断。
//
// 返回 {output, exit_code}：exit_code = 子进程退出码（execvp 失败=127，正常=脚本 exit）。
// 上层 MUST 检 exit_code != 0 → 失败，不能只看 stdout 字串（空输出+exit 127 会被静默吞）。
// 这修了 skills sync 静默失败的真根因：daemon 启动时 PATH 无 python3，execvp 失败 _exit(127)，
// 父进程拿到空串，上层只检 ERROR/FATAL/Traceback 不命中 → 静默吞 → 沙箱里 harness/skills 空。
// 与 sandbox_types.h 的 ExecResult（工具执行结果，对外暴露）撞名，本 struct 仅本文件内部用
struct SubprocResult {
  std::string output;
  int exit_code;  // -1 = pipe/fork 失败（父进程侧资源错）
};
static SubprocResult exec_cmd_argv(const std::vector<std::string>& argv_strs) {
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

E2BSandbox::E2BSandbox(const SandboxKey& owner,
                         const std::string& workdir_root,
                         const std::string& sandbox_http_server_path,
                         int sandbox_http_port,
                         int64_t output_max_bytes,
                         const std::string& skills_src_dir,
                         const std::string& e2b_remote_workspace_dir,
                         const std::string& e2b_api_url,
                         const std::string& e2b_api_key,
                         const std::string& e2b_default_template,
                         const std::string& e2b_network_allow_out,
                         const std::string& e2b_ca_cert_path,
                         const std::string& agent_package_dir)
    : owner_(owner),
      sandbox_id_(generate_sandbox_id()),
      workdir_(fs::absolute(workdir_root).string() + "/" + sandbox_id_),
      output_max_bytes_(output_max_bytes),
      state_(SandboxState::RUNNING),
      traffic_access_token_(generate_token()),
      e2b_sync_script_path_(sandbox_http_server_path),
      skills_src_dir_(skills_src_dir),
      remote_workspace_dir_(e2b_remote_workspace_dir),
      e2b_api_url_(e2b_api_url),
      e2b_api_key_(e2b_api_key),
      e2b_default_template_(e2b_default_template),
      e2b_network_allow_out_(e2b_network_allow_out),
      e2b_ca_cert_path_(e2b_ca_cert_path),
      agent_package_dir_(agent_package_dir) {
  // 允许通过环境变量覆盖远端 workspace 路径（优先级：环境变量 > 配置文件 > 代码默认值）
  const char* env_remote_dir = getenv("AF_SANDBOX_E2B_REMOTE_WORKSPACE_DIR");
  if (env_remote_dir && strlen(env_remote_dir) > 0) {
    remote_workspace_dir_ = env_remote_dir;
  }
  // 创建 workdir（宿主机侧，用于存放待上传/已下载文件）
  std::error_code ec;
  fs::create_directories(workdir_, ec);
  if (ec) {
    throw SandboxFactory::SandboxLaunchException(
        "Failed to create workdir: " + workdir_ + " (" + ec.message() + ")");
  }

  // 获取 E2B_API_KEY：配置文件 e2b_api_key > 环境变量 E2B_API_KEY
  std::string api_key;
  if (!e2b_api_key_.empty()) {
    api_key = e2b_api_key_;
  } else {
    const char* env_key = getenv("E2B_API_KEY");
    if (env_key && strlen(env_key) > 0) {
      api_key = env_key;
    }
  }
  if (api_key.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B_API_KEY not set (config e2b_api_key or env E2B_API_KEY)");
  }
  // 设置环境变量，子进程（e2b_lifecycle.py）继承
  setenv("E2B_API_KEY", api_key.c_str(), 1);

  // 通过外部 Python 脚本 e2b_lifecycle.py 创建沙箱并启动 HTTP server
  // 脚本与 e2b_sync.py 同级（src/sandbox/ 目录），直指绝对路径
  fs::path e2b_sandbox_dir = fs::absolute("src/sandbox");
  fs::path lifecycle_script = e2b_sandbox_dir / "e2b_lifecycle.py";

  if (!fs::exists(lifecycle_script)) {
    throw SandboxFactory::SandboxLaunchException(
        "e2b_lifecycle.py not found: " + lifecycle_script.string());
  }

  // 设置 E2B_API_URL 环境变量，SDK 子进程自动继承
  if (!e2b_api_url_.empty()) {
    setenv("E2B_API_URL", e2b_api_url_.c_str(), 1);
  }

  // 设置 SSL_CERT_FILE 环境变量（自建部署自签名证书），子进程（e2b_lifecycle.py）继承
  // 将相对路径解析为绝对路径，避免子进程 CWD 不一致导致加载失败
  if (!e2b_ca_cert_path_.empty()) {
    std::string abs_cert_path = fs::absolute(e2b_ca_cert_path_).string();
    setenv("SSL_CERT_FILE", abs_cert_path.c_str(), 1);
  }

  std::vector<std::string> create_argv = {
      resolve_python3(), lifecycle_script.string(),
      "create",
      "--traffic-access-token", traffic_access_token_,
      "--sandbox-http-server-path", sandbox_http_server_path,
      "--sandbox-http-port", std::to_string(sandbox_http_port),
      "--remote-workspace-dir", remote_workspace_dir_,
      "--template", e2b_default_template_,
  };
  if (!e2b_network_allow_out_.empty()) {
    create_argv.push_back("--network-allow-out");
    create_argv.push_back(e2b_network_allow_out_);
  }
  auto create_res = exec_cmd_argv(create_argv);
  if (create_res.exit_code != 0 || create_res.output.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B Sandbox creation failed: python3 exit=" +
        std::to_string(create_res.exit_code) +
        " output=" + create_res.output);
  }
  std::string output = create_res.output;

  // 解析 JSON 输出拿 sandbox_id 和 endpoint
  if (output.find("Error") != std::string::npos ||
      output.find("Traceback") != std::string::npos) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B Sandbox creation failed: " + output);
  }

  // stderr 合并到 stdout（line 69），日志行在前、JSON 在最后一行
  // 取最后非空行解析，不直接 parse 整个混合输出
  std::string json_line;
  {
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
      auto start = line.find_first_not_of(" \t\r\n");
      if (start != std::string::npos && line[start] == '{') {
        json_line = line;
      }
    }
  }
  if (json_line.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B Sandbox creation failed: no JSON line in output: " + output);
  }
  rapidjson::Document doc;
  doc.Parse(json_line.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B Sandbox creation failed: invalid JSON output: " + output);
  }
  if (doc.HasMember("id") && doc["id"].IsString()) {
    sandbox_id_ = doc["id"].GetString();
  }
  if (doc.HasMember("host_url") && doc["host_url"].IsString()) {
    endpoint_ = doc["host_url"].GetString();
  }
  if (sandbox_id_.empty() || endpoint_.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B Sandbox creation failed: missing id or host_url in: " + output);
  }

  // ── workspace 同步闭环（Gap 1 修复）──────────────────────────────
  // E2B 沙箱在远端，必须主动上传宿主 workdir 内容
  // 失败 fail-fast（B22 不 fallback）：上传失败抛 SandboxLaunchException
  // 脚本不存在时静默跳过：允许纯 HTTP 模式（无文件同步）
  upload_via_e2b_sync(workdir_, remote_workspace_dir_, "workspace upload");

  // ── agent 包根整体同步（manifest 驱动，设计文档 sandbox-env-injection.md §2 链路环节⑤）──
  // 此前实现只同步 harness/skills 子集，环节⑤未落地 → .env、agent.md、nodes/、
  // harness/tools/、workflows/ 等包根文件从未进沙箱。e2b.py 的 set -a && source
  // /home/user/workspace/.env 兜底跳过 → 密钥缺失 → 子进程 os.environ.get() 读不到。
  //
  // 不走"盲传整个包根"——那会把 .venv(91M)/workspace/outputs/tests 等沙箱无关产物
  // 全传上去（agent-package-structure.md §3 设计约束：只传 manifest 声明的内容）。
  // 此段调 e2b_sync.py upload --manifest <agent_package_dir>/manifest.jsonc：
  //   - manifest.jsonc 存在 → 按 includes/excludes 白名单过滤上传（用户声明什么传什么）
  //   - manifest.jsonc 不存在 → e2b_sync.py 用 agentflow_runtime.common.manifest 框架规则
  //     推断生成（首次连接），后续连接直接读，跳过重新推断（§3.1 场景①②）
  //   - manifest 加载/生成失败 → e2b_sync.py FATAL exit 1 → 这里 fail-fast（B22 不 fallback）
  //
  // 与下游 skills 段不冲突：skills 段是 harness/skills 子集，e2b_sync.py 幂等跳过同内容。
  if (!agent_package_dir_.empty()) {
    std::string manifest_path =
        (fs::path(agent_package_dir_) / "manifest.jsonc").string();
    upload_via_e2b_sync(agent_package_dir_, remote_workspace_dir_,
                        "agent package upload", manifest_path);
  }

  // ── skills 同步到远端沙箱 ──────────────────────────────────────
  // 将宿主机 harness/skills 复制到远端沙箱的 workdir 下
  // 失败 fail-fast（B22 不 fallback）
  if (!skills_src_dir_.empty() && fs::is_directory(skills_src_dir_)) {
    upload_via_e2b_sync(skills_src_dir_,
                        remote_workspace_dir_ + "/harness/skills",
                        "skills sync");
  }
  mark_active();  // FR-009：初始化活跃时间戳
}

// ── 私有构造器：不调 e2b_lifecycle.py create，直接注入已 fork 的结果 ─────
//
// 仅 ForkFromSnapshot 静态工厂使用，跳过全量 create 流程（E2B 平台侧
// e2b_lifecycle.py fork 已完成沙箱派生，C++ 侧只设成员变量）。
E2BSandbox::E2BSandbox(const SandboxKey& owner,
                        const std::string& workdir,
                        const std::string& forked_sandbox_id,
                        const std::string& forked_endpoint,
                        const std::string& sandbox_http_server_path,
                        int sandbox_http_port,
                        int64_t output_max_bytes,
                        const std::string& skills_src_dir,
                        const std::string& e2b_remote_workspace_dir,
                        const std::string& e2b_api_url,
                        const std::string& e2b_api_key,
                        const std::string& e2b_default_template,
                        const std::string& e2b_network_allow_out,
                        const std::string& e2b_ca_cert_path,
                        const std::string& agent_package_dir)
    : owner_(owner),
      sandbox_id_(forked_sandbox_id),
      workdir_(workdir),
      endpoint_(forked_endpoint),
      traffic_access_token_(generate_token()),
      e2b_sync_script_path_(sandbox_http_server_path),
      skills_src_dir_(skills_src_dir),
      remote_workspace_dir_(e2b_remote_workspace_dir),
      e2b_api_url_(e2b_api_url),
      e2b_api_key_(e2b_api_key),
      e2b_default_template_(e2b_default_template),
      e2b_network_allow_out_(e2b_network_allow_out),
      e2b_ca_cert_path_(e2b_ca_cert_path),
      agent_package_dir_(agent_package_dir),
      output_max_bytes_(output_max_bytes),
      state_(SandboxState::RUNNING) {
  // Fork 结果验证——API key 检查
  std::string api_key = e2b_api_key_;
  if (api_key.empty()) {
    const char* env_key = getenv("E2B_API_KEY");
    if (env_key && strlen(env_key) > 0) api_key = env_key;
  }
  if (api_key.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B_API_KEY not set (config e2b_api_key or env E2B_API_KEY)");
  }
  setenv("E2B_API_KEY", api_key.c_str(), 1);
  if (!e2b_api_url_.empty()) setenv("E2B_API_URL", e2b_api_url_.c_str(), 1);
  if (!e2b_ca_cert_path_.empty()) {
    setenv("SSL_CERT_FILE", fs::absolute(e2b_ca_cert_path_).string().c_str(), 1);
  }

  // fork 沙箱的 filesystem 继承源沙箱，不需要重新 Skills/workspace 同步。
  // HTTP server 已由 e2b_lifecycle.py fork 在远端启动并验证就绪。
  mark_active();  // FR-009：初始化活跃时间戳
}

// ── 静态工厂：从源沙箱 fork 派生新沙箱（B3 restore 通道） ─────────────────
//
// 调 e2b_lifecycle.py fork 在 E2B 平台侧派生新沙箱，filesystem 继承自源沙箱。
// 返回的沙箱已就绪（HTTP server 已在跑），不需要重新 Skills/workspace 同步。
std::unique_ptr<E2BSandbox> E2BSandbox::ForkFromSnapshot(const ForkParams& params) {
  // 1. 创建宿主 workdir
  std::string sandbox_id = generate_sandbox_id();
  std::string workdir = fs::absolute(params.workdir_root).string() + "/" + sandbox_id;
  std::error_code ec;
  fs::create_directories(workdir, ec);
  if (ec) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: failed to create workdir: " + workdir + " (" + ec.message() + ")");
  }

  // 2. 获取 API key（config → env var）
  std::string api_key = params.e2b_api_key;
  if (api_key.empty()) {
    const char* env_key = getenv("E2B_API_KEY");
    if (env_key && strlen(env_key) > 0) {
      api_key = env_key;
    }
  }
  if (api_key.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: E2B_API_KEY not set");
  }
  setenv("E2B_API_KEY", api_key.c_str(), 1);
  if (!params.e2b_api_url.empty()) {
    setenv("E2B_API_URL", params.e2b_api_url.c_str(), 1);
  }
  if (!params.e2b_ca_cert_path.empty()) {
    std::string abs_cert = fs::absolute(params.e2b_ca_cert_path).string();
    setenv("SSL_CERT_FILE", abs_cert.c_str(), 1);
  }

  fs::path lifecycle_script = fs::absolute("src/sandbox/e2b_lifecycle.py");
  if (!fs::exists(lifecycle_script)) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: e2b_lifecycle.py not found: " + lifecycle_script.string());
  }

  // 4. 调 e2b_lifecycle.py fork
  std::vector<std::string> fork_argv = {
      resolve_python3(), lifecycle_script.string(),
      "fork",
      "--sandbox-id", params.source_sandbox_id,
      "--sandbox-http-server-path", params.sandbox_http_server_path,
      "--sandbox-http-port", std::to_string(params.sandbox_http_port),
  };
  auto fork_res = exec_cmd_argv(fork_argv);
  if (fork_res.exit_code != 0 || fork_res.output.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: fork failed (exit=" +
        std::to_string(fork_res.exit_code) + "): " + fork_res.output);
  }
  std::string output = fork_res.output;
  if (output.find("Error") != std::string::npos ||
      output.find("Traceback") != std::string::npos) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: fork failed: " + output);
  }

  // 5. 解析 JSON 输出（取最后 JSON 行，与 create 类似）
  std::string json_line;
  {
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
      auto start = line.find_first_not_of(" \t\r\n");
      if (start != std::string::npos && line[start] == '{') {
        json_line = line;
      }
    }
  }
  if (json_line.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: no JSON line in output: " + output);
  }
  rapidjson::Document doc;
  doc.Parse(json_line.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: invalid JSON output: " + output);
  }
  std::string forked_id;
  std::string host_url;
  if (doc.HasMember("id") && doc["id"].IsString()) {
    forked_id = doc["id"].GetString();
  }
  if (doc.HasMember("host_url") && doc["host_url"].IsString()) {
    host_url = doc["host_url"].GetString();
  }
  if (forked_id.empty() || host_url.empty()) {
    throw SandboxFactory::SandboxLaunchException(
        "ForkFromSnapshot: missing id or host_url in: " + output);
  }

  // 6. 构造 E2BSandbox 实例——用私有构造注入 fork 结果
  // 使用 private 构造器（仅此工厂函数可访问），绕过全量 create 流程
  return std::unique_ptr<E2BSandbox>(
      new E2BSandbox(
          params.owner,
          workdir,                   // 宿主 workdir（直接传入已构造的路径）
          forked_id,                 // E2B 平台侧 sandbox_id
          host_url,                  // host_url
          params.sandbox_http_server_path,
          params.sandbox_http_port,
          params.output_max_bytes,
          "",                        // skills_src_dir：空 = 由 filesystem 继承
          params.remote_workspace_dir,
          params.e2b_api_url,
          params.e2b_api_key,
          params.e2b_default_template,
          params.e2b_network_allow_out,
          params.e2b_ca_cert_path,
          ""));                       // agent_package_dir：空 = fork 分支 filesystem 继承自源沙箱
}

E2BSandbox::~E2BSandbox() {
  if (state_ == SandboxState::RUNNING) {
    destroy();
  }
}

std::string E2BSandbox::generate_token() {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device rd;
  std::stringstream ss;
  ss << std::hex << now << "-" << rd() << "-" << rd();
  return ss.str();
}

std::string E2BSandbox::curl_post_with_token(const std::string& path,
                                              const std::string& body_json,
                                              int64_t timeout_ms) {
  // 安全版本：fork+execvp 数组形式 + stdin 管道传 body（避免 shell 注入，CWE-77）
  // body 通过 --data-binary @- 从 stdin 读取，不经过命令行参数
  // 与 LocalSandbox::curl_post 保持一致的 fork+exec 模式（F5 单一真相来源）
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
    // ── 子进程 ────────────────────────────────────────────
    // 重定向 stdin/stdout
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[0]); close(stdin_pipe[1]);
    close(stdout_pipe[0]); close(stdout_pipe[1]);

    // curl 参数数组（execvp 要求 nullptr 结尾）
    int timeout_s = static_cast<int>((timeout_ms + 999) / 1000);
    std::string timeout_str = std::to_string(timeout_s + 5);
    std::string auth_header = "Authorization: Bearer " + traffic_access_token_;

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("curl"));
    argv.push_back(const_cast<char*>("-s"));
    argv.push_back(const_cast<char*>("-m"));
    argv.push_back(const_cast<char*>(timeout_str.c_str()));
    argv.push_back(const_cast<char*>("-X"));
    argv.push_back(const_cast<char*>("POST"));
    argv.push_back(const_cast<char*>("-H"));
    argv.push_back(const_cast<char*>("Content-Type: application/json"));
    argv.push_back(const_cast<char*>("-H"));
    argv.push_back(const_cast<char*>(auth_header.c_str()));
    argv.push_back(const_cast<char*>("--data-binary"));
    argv.push_back(const_cast<char*>("@-"));  // 从 stdin 读 body
    argv.push_back(const_cast<char*>(path.c_str()));
    argv.push_back(nullptr);

    execvp("curl", argv.data());
    _exit(127);  // execvp 仅在失败时返回
  }

  // ── 父进程 ────────────────────────────────────────────
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);

  // 写 body 到 stdin（一次性写，curl 端读取）
  ssize_t total_written = 0;
  while (total_written < static_cast<ssize_t>(body_json.size())) {
    ssize_t n = write(stdin_pipe[1],
                      body_json.data() + total_written,
                      body_json.size() - total_written);
    if (n <= 0) break;
    total_written += n;
  }
  close(stdin_pipe[1]);  // 关 stdin 发 EOF

  // 读 stdout
  std::string resp;
  char buf[4096];
  ssize_t n;
  while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
    resp.append(buf, n);
  }
  close(stdout_pipe[0]);

  // 等待子进程退出（避免僵尸进程）
  int status;
  waitpid(pid, &status, 0);

  return resp;
}

ExecResult E2BSandbox::exec(const std::string& tool,
                            const std::string& tool_params_json,
                            const ExecOptions& opts) {
  if (state_ != SandboxState::RUNNING) {
    ExecResult r;
    r.exit_code = 1;
    r.stderr = "Sandbox not RUNNING (state=" + std::to_string(static_cast<int>(state_)) + ")";
    return r;
  }

  int64_t timeout_ms = opts.timeout_ms > 0 ? opts.timeout_ms : 30000;
  std::string path = endpoint_ + "/exec";
  std::string resp = curl_post_with_token(path, tool_params_json, timeout_ms);

  mark_active();  // FR-009：每次 exec 更新活跃时间
  return SandboxHelpers::parse_exec_result_json(resp, endpoint_);
}

void E2BSandbox::snapshot() {
  // FR-012：通过 e2b_lifecycle.py snapshot 在 E2B 平台侧保存沙箱磁盘状态
  // 幂等：已 RECYCLING 的沙箱跳过（kill 后 snapshot API 不可用）
  if (state_ == SandboxState::RECYCLING) return;

  fs::path lifecycle_script = fs::absolute("src/sandbox/e2b_lifecycle.py");

  if (fs::exists(lifecycle_script)) {
    std::vector<std::string> snap_argv = {
        resolve_python3(), lifecycle_script.string(),
        "snapshot",
        "--sandbox-id", sandbox_id_,
    };
    auto snap_res = exec_cmd_argv(snap_argv);
    // 记录 snapshot 子进程输出（含错误），失败不抛异常
    // exit_code=127 = python3 不在 PATH（execvp 失败）→ 也要 warn，否则静默
    if (snap_res.exit_code != 0 ||
        snap_res.output.find("ERROR") != std::string::npos ||
        snap_res.output.find("FATAL") != std::string::npos ||
        snap_res.output.find("Traceback") != std::string::npos) {
      Logger::instance().warn_f("[E2B] snapshot failed for sandbox=%s (exit=%d): %s",
                               sandbox_id_.c_str(), snap_res.exit_code,
                               snap_res.output.c_str());
    } else if (!snap_res.output.empty()) {
      Logger::instance().info_f("[E2B] snapshot done for sandbox=%s", sandbox_id_.c_str());
    }
  }
}

void E2BSandbox::upload_via_e2b_sync(const std::string& local_dir,
                                     const std::string& remote_dir,
                                     const std::string& err_prefix,
                                     const std::string& manifest_path) {
  // e2b_sync.py upload 的统一执行器——收敛 ctor/sync_session_uploads 四处同款调用
  // 消除"霰弹式修改"反模式（四处各自 fs::absolute("src/sandbox/e2b_sync.py") + argv + 同款判定）：
  //   - sync_script 路径直指 src/sandbox/e2b_sync.py（daemon cwd 恒为项目根，
  //     fs::absolute("src/...") 是全局既有约定，见 sandbox_lifecycle.cpp:63 http_server_path）
  //   - 失败判定口径与原四处逐字一致：exit_code != 0 或 output 含 ERROR/FATAL/Traceback → 抛异常
  //   - manifest_path 非空时透传给 e2b_sync.py --manifest 走白名单过滤（仅 agent 包根段用）
  //
  // 路径推导修正（2026-08-04）：
  //   曾用 fs::path(e2b_sync_script_path_).parent_path() / "e2b_sync.py" 同源推导，
  //   但该假设"sandbox_http_server.py 与 e2b_sync.py 同目录"不成立——
  //   sandbox_http_server.py 在 docker/sandbox/http_server/（沙箱内 HTTP server），
  //   e2b_sync.py 在 src/sandbox/（宿主机同步脚本），两者不同目录！
  //   parent_path 推导出 docker/sandbox/http_server/e2b_sync.py（不存在）→
  //   skills sync 段 err_prefix 含 "sync" → throw → daemon 崩（libc++abi terminate，
  //   sidecar 模拟 SSE 收到 "daemon WebSocket closed"，本任务排查的卡住根因）。
  // 修正：直指 src/sandbox/e2b_sync.py（与原四处调用逐字一致）。
  //
  // e2b_sync_script_path_ 空串 = 纯 HTTP 模式（无文件同步），保持原"静默跳过"行为
  if (e2b_sync_script_path_.empty()) return;

  fs::path sync_script = fs::absolute("src/sandbox/e2b_sync.py");
  if (!fs::exists(sync_script)) {
    // 脚本不存在时静默跳过：允许纯 HTTP 模式（无文件同步）
    // 与原四处行为逐字一致（原 ctor 三段静默跳过，sync_session_uploads段 throws——
    // 此差异保留：err_prefix 含 "sync" 视为 sync_session_uploads 调用方，throws on missing）
    if (err_prefix.find("sync") != std::string::npos) {
      throw std::runtime_error(
          "E2BSandbox::" + err_prefix + ": e2b_sync.py not found: " + sync_script.string());
    }
    return;
  }

  std::vector<std::string> argv = {
      resolve_python3(), sync_script.string(),
      "upload",
      "--sandbox-id", sandbox_id_,
      "--local-dir", local_dir,
      "--remote-dir", remote_dir,
  };
  if (!manifest_path.empty()) {
    argv.push_back("--manifest");
    argv.push_back(manifest_path);
  }

  auto res = exec_cmd_argv(argv);
  // 失败判定：exit_code != 0 或输出含 ERROR/FATAL/Traceback（口径与原四处逐字一致）
  // exit_code=127 = execvp 失败（python3 不在 PATH）→ 必须当错，否则静默"成功"但无数据传输
  if (res.exit_code != 0 ||
      res.output.find("ERROR") != std::string::npos ||
      res.output.find("FATAL") != std::string::npos ||
      res.output.find("Traceback") != std::string::npos) {
    throw SandboxFactory::SandboxLaunchException(
        "E2B " + err_prefix + " failed (exit=" +
        std::to_string(res.exit_code) + "): " + res.output);
  }
}

void E2BSandbox::sync_session_uploads(const std::string& session_upload_dir) {
  // 增量同步会话级用户上传目录到远端沙箱（agent-package-structure.md §2.2/§3.2）
  // 调 e2b_sync.py upload 把宿主机 session_upload_dir 同步到
  // {remote_workspace_dir}/workspace/{user_id}_{client_id}/upload
  //
  // 幂等：e2b_sync.py upload 对已存在同内容文件跳过（content-addressed）
  // 失败抛 std::runtime_error 含可定位信息，daemon 捕获后记 warning，不拒绝请求
  if (state_ != SandboxState::RUNNING) {
    throw std::runtime_error(
        "E2BSandbox::sync_session_uploads: sandbox not RUNNING (state=" +
        std::to_string(static_cast<int>(state_)) + ")");
  }
  if (session_upload_dir.empty()) {
    throw std::runtime_error(
        "E2BSandbox::sync_session_uploads: session_upload_dir is empty");
  }
  std::error_code ec;
  if (!fs::is_directory(session_upload_dir, ec)) {
    // 目录不存在 = 本轮无用户上传，no-op（不是错误）
    Logger::instance().info_f(
        "[E2B] sync_session_uploads skipped: session_upload_dir not exist: %s",
        session_upload_dir.c_str());
    return;
  }

  // remote 目标 = 远端 workspace 根 / 同名相对路径
  // session_upload_dir 形如 .../{agent_id}/workspace/{user_id}_{client_id}/upload
  // 我们取 .../{agent_id}/ 之后的相对部分拼到 remote_workspace_dir_ 后面
  std::string rel = session_upload_dir;
  const std::string marker = "/workspace/";
  auto pos = rel.find(marker);
  if (pos == std::string::npos) {
    throw std::runtime_error(
        "E2BSandbox::sync_session_uploads: session_upload_dir missing '/workspace/' marker: "
        + rel);
  }
  // 跳过 marker 的前导 '/'，rel_under_workspace 形如 "workspace/{session}/upload"（无前导 /）
  // 避免拼 remote_target 时出现 "//" 双斜线
  std::string rel_under_workspace = rel.substr(pos + 1);
  std::string remote_target = remote_workspace_dir_ + "/" + rel_under_workspace;

  // 复用统一 upload 执行器（err_prefix 含 "sync" → 脚本缺失时 throws 而非静默跳过，
  // 保持 sync_session_uploads 段原严格行为）
  upload_via_e2b_sync(session_upload_dir, remote_target, "sync_session_uploads");

  Logger::instance().info_f(
      "[E2B] sync_session_uploads done: %s -> %s",
      session_upload_dir.c_str(), remote_target.c_str());
}

void E2BSandbox::destroy() {
  // B7：幂等——多次调用一致，sandbox 已 kill 时无操作
  if (state_ == SandboxState::RECYCLING) return;

  // Provider 级 snapshot 由 SandboxLifecycle::destroy() 统一调用，此处不重复
  // （FR-012：避免双重 snapshot API 调用）

  state_ = SandboxState::RECYCLING;

  // 通过外部 Python 脚本 e2b_lifecycle.py 幂等销毁沙箱
  fs::path lifecycle_script = fs::absolute("src/sandbox/e2b_lifecycle.py");

  if (fs::exists(lifecycle_script)) {
    std::vector<std::string> kill_argv = {
        resolve_python3(), lifecycle_script.string(),
        "kill",
        "--sandbox-id", sandbox_id_,
    };
    (void)exec_cmd_argv(kill_argv);
  }
}

}  // namespace sandbox
}  // namespace agentflow