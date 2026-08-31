// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DaemonConfig - Configuration for agentflowd daemon

#ifndef AGENTFLOW_DAEMON_CONFIG_H
#define AGENTFLOW_DAEMON_CONFIG_H

#include <string>
#include <vector>

#include "runtime/agent_runtime.h"
#include "runtime/master/deployer.h"
#include "runtime/master/scheduler.h"

namespace agentflow {
namespace daemon {

struct DaemonConfig {
  // Server
  std::string listen = "0.0.0.0:7890";
  // `cli_listen_set` 标记 `--listen` CLI arg 是否被显式设过。load_config_file() 据此跳过
  // 用 JSON 配置文件的 listen 字段覆盖 CLI arg——守护「CLI > JSON」优先级不变量
  // （docs/design/config-landscape.md §2.1.4）。见 config.cpp:288-289。
  bool cli_listen_set = false;
  std::string uds_biz_path = "/tmp/agentflowd_biz.sock";
  std::string uds_ctrl_path = "/tmp/agentflowd_ctrl.sock";
  int max_connections = 1024;

  // Runtime (embeds existing configs)
  runtime::MMAgentRuntime::Config runtime_config;
  runtime::master::Deployer::Config deployer_config;
  runtime::master::Scheduler::Config scheduler_config;

  // Agents
  struct AgentEntry {
    std::string package_path;
    std::string name_override;
  };
  std::vector<AgentEntry> agents;

  // Logging
  std::string log_level = "info";
  std::string log_file;  // empty = stdout only

  // Flow trace
  std::string flow_trace_file;  // single .ndjson file (all events appended)
  std::string flow_trace_dir;   // per-session .ndjson files under this directory

  // Span trace（TraceSystem NDJSON 输出目录，C++ TraceManager 与 Python SpanWriter 共用）
  // 空字符串 = 使用代码默认 "trace_output"（相对 daemon 启动 cwd）
  std::string trace_output_dir;

  // Daemon
  std::string pid_file;
  bool daemonize = false;
  int drain_timeout_ms = 30000;

  // DataRef Bridge (总线 B) configuration
  struct DataRefBridgeConfig {
    int         capacity_mb = 512;
    std::string redis_url   = "redis://127.0.0.1:6379";
  };
  DataRefBridgeConfig dataref_bridge_config;

  // Sandbox (AgentFoundry 沙箱机制移植)
  struct SandboxConfig {
    // mode: "none" | "local" | "docker" | "e2b"
    // none = 向后兼容，nullptr Sandbox，宿主机直接执行
    std::string mode = "none";

    // 并发软上限（SC-011）：超限拒绝新用户对话直至有沙箱被回收
    int sandbox_max_active = 16;

    // GC 周期与销毁 TTL（FR-009）：活沙箱闲置超 gc_destroy_ttl_ms 转 RECYCLING
    int64_t gc_destroy_ttl_ms = 3600000;  // 1 h; longer than one dialogue timeout

    // 快照过期清理 TTL（FR-009）：销毁后快照超 gc_recycle_snapshot_ttl_ms 清理
    int64_t gc_recycle_snapshot_ttl_ms = 86400000;  // 24 h

    // 输出截断阈值（FR-005）：超 output_max_bytes 截断 stdout/stderr，truncated=true
    int64_t output_max_bytes = 0;  // 0 = 不截断（向后兼容）

    // 沙箱工作区根目录：每沙箱在 workdir_root/<sandbox_id>/ 下
    std::string workdir_root = "/tmp/agentflow_sandbox";

    // 快照持久化根目录：销毁时快照落 snapshot_root/<sandbox_id>/
    std::string snapshot_root = "/tmp/agentflow_snapshots";

    // 沙箱内 HTTP server 监听端口（容器内固定，宿主机侧 LocalSandbox 用 0 自动分配）
    int sandbox_http_port = 8080;

    // E2B 远程沙箱配置（mode=e2b 时生效）
    // E2B API 服务端点 URL（如 http://218.245.96.148:30030）
    // 对应 SDK 环境变量 E2B_API_URL，子进程继承
    std::string e2b_api_url;

    // E2B API 鉴权密钥（对应环境变量 E2B_API_KEY）
    // 优先级：配置文件 e2b_api_key > 环境变量 E2B_API_KEY
    std::string e2b_api_key;

    // E2B 沙箱模板 ID（如 tpl-cb6b9dcbf5784a1487a75971）
    // 传给 Sandbox.create(template=...)
    std::string e2b_default_template = "base";

    // E2B CA 证书路径（自建部署自签名证书，如 configs/certs/e2b_CA.pem）
    // 设置后由 daemon 设为 SSL_CERT_FILE 环境变量，子进程（e2b_lifecycle.py）继承
    // 空字符串 = 不设置（使用系统默认 CA 证书链）
    std::string e2b_ca_cert_path;

    // E2B 沙箱网络出站规则（JSON 数组，如 ["10.0.0.0/8","218.0.0.0/8"]）
    // 传给 Sandbox.create(network={allow_out: ...})
    // 空字符串 = 不限制
    std::string e2b_network_allow_out;

    // E2B 远端沙箱内 workspace 挂载点
    // 默认与 E2B 基础镜像一致（/home/user/workspace）
    // 可通过 AF_SANDBOX_E2B_REMOTE_WORKSPACE_DIR 环境变量覆盖
    std::string e2b_remote_workspace_dir = "/home/user/workspace";
    // agent 级快照映射持久化文件路径（agent_id → snapshotID + version/hash）
    // 默认 data/template_registry.json（与 sessions.db 同目录，运行时状态归 data 非 logs）
    // 可通过 AF_SANDBOX_E2B_TEMPLATE_MAP_PATH 环境变量覆盖
    std::string e2b_template_map_path = "data/template_registry.json";

    // 宿主机上 agent package 的 harness/skills 路径，用于 DockerSandbox/E2B skills-sync
    // 由 daemon 启动时根据 --package 路径自动推导（package_path + "/harness/skills"）
    // 可通过 AF_SANDBOX_SKILLS_SRC_DIR 环境变量覆盖
    std::string skills_src_dir;

    // Docker/E2B 沙箱镜像标签（sandbox_docker.h 默认值为 "agentflow-sandbox:v1"）
    // 可通过 AF_SANDBOX_IMAGE_NAME 环境变量覆盖
    std::string image_name = "agentflow-sandbox:v1";

    // 沙箱降级开关：true=拉起失败时降级到无沙箱继续执行（当前行为）
    //               false=拉起失败时直接拒绝请求，返回明确错误
    // 开发环境建议设为 false，以便尽早发现沙箱不可用问题
    bool allow_fallback = true;
  };
  SandboxConfig sandbox_config;

  // Runtime package directory (e.g. packages/agentflow-runtime).
  // If set, daemon runs `pip install -e <dir>` at startup to pre-install
  // agentflow-runtime in the default Python interpreter, providing a
  // fallback when the agent's .venv is unavailable or incomplete.
  std::string runtime_pkg_dir;

  // CLI-only fields (not in config file)
  std::string config_file;
  bool validate = false;
};

// Parse CLI arguments into DaemonConfig. Returns true on success.
bool parse_cli_args(int argc, char* argv[], DaemonConfig& config);

// Apply environment variable overrides to DaemonConfig.
void apply_env_overrides(DaemonConfig& config);

// Load JSON config file and merge into DaemonConfig (file values fill in defaults,
// but CLI/env values take precedence — call this before apply_env_overrides).
bool load_config_file(DaemonConfig& config);

// Merge all sources: CLI > ENV > File > Default. Call order:
//   1. parse_cli_args (sets CLI values)
//   2. load_config_file (sets file values for unset fields)
//   3. apply_env_overrides (overrides with env for unset fields)
bool merge_config(DaemonConfig& config);

// Validate the final configuration.
bool validate_config(const DaemonConfig& config);

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_CONFIG_H
