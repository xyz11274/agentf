// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Daemon implementation - agentflowd orchestrator

#include "daemon/daemon.h"  // NOLINT: resolved via include/internal/

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

#include "agent/agent.h"
#include "agent/dataref_bridge.h"
#include "agent/package.h"
#include "daemon/agent_registry.h"  // NOLINT
#include "daemon/rpc_handler.h"     // NOLINT
#include "daemon/rpc_server.h"      // NOLINT
#include "daemon/response_poller.h" // NOLINT
#include "daemon/event_broker.h"    // NOLINT // NOLINT
#include "daemon/sandbox_lifecycle.h"   // SandboxLifecycle 完整类型（unique_ptr 析构需要）
#include "daemon/sandbox_snapshots.h"   // SandboxSnapshots 完整类型
#include "daemon/session_manager.h"     // SessionManager 完整类型
#include "daemon/template_registry.h"   // TemplateRegistry 完整类型（v2 段1 预热）
#include "runtime/agent_runtime.h"
#include "runtime/flow_trace.h"
#include "runtime/trace/trace_manager.h"
#include "utils/logger.h"

namespace agentflow {
namespace daemon {

static std::atomic<ShutdownPhase> g_shutdown_phase{ShutdownPhase::NONE};
static Daemon* g_daemon = nullptr;

static void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    if (g_shutdown_phase == ShutdownPhase::NONE) {
      g_shutdown_phase = ShutdownPhase::DRAINING;
      if (g_daemon) g_daemon->shutdown();
    } else {
      g_shutdown_phase = ShutdownPhase::FORCED;
      if (g_daemon) g_daemon->force_shutdown();  // FR-023: quick cleanup before _exit
      _exit(1);
    }
  }
}

static void install_signal_handlers() {
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

static int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Daemon::Daemon(const DaemonConfig& config) : config_(config) {}

Daemon::~Daemon() {
  if (g_daemon == this) g_daemon = nullptr;
  shutdown();
  // Properly clean up services that were partially started.
  // This ensures threads are joined before static destruction begins —
  // preventing "mutex lock failed: Invalid argument" on macOS/libc++
  // (F-20260704-001).
  shutdown_services();
}

bool Daemon::initialize() {
  g_daemon = this;
  install_signal_handlers();

  // 0. Clean up orphan sandbox resources from previous crash (FR-025 L3)
  // 仅在沙箱模式启用时清理，mode=none 时跳过（避免误杀其他项目同名进程）
  if (config_.sandbox_config.mode != "none") {
    cleanup_orphan_sandboxes();
  }

  // 1. Initialize logging
  {
    Logger::Level lvl = Logger::Level::INFO;
    if (config_.log_level == "trace" || config_.log_level == "debug")
      lvl = Logger::Level::DEBUG;
    else if (config_.log_level == "warn")
      lvl = Logger::Level::WARN;
    else if (config_.log_level == "error")
      lvl = Logger::Level::ERROR;
    Logger::instance().set_level(lvl);
    Logger::instance().set_log_rotation(50 * 1024 * 1024, 5);
    if (!config_.log_file.empty()) {
      Logger::instance().set_log_file(config_.log_file);
    }
  }

  // 1b. Initialize flow trace
  {
    auto& flow_trace = runtime::FlowTraceWriter::instance();
    if (!config_.flow_trace_dir.empty()) {
      flow_trace.set_output_dir(config_.flow_trace_dir);
      flow_trace.set_stdout_enabled(false);
    } else if (!config_.flow_trace_file.empty()) {
      flow_trace.set_output_file(config_.flow_trace_file);
      flow_trace.set_stdout_enabled(false);
    }
  }

  // 1c. Initialize span trace（TraceSystem NDJSON 输出目录）
  // 与 FlowTrace 不同：trace_output_dir 是 Python SpanWriter（AF_TRACE_OUTPUT_DIR）
  // 与 C++ TraceManager 的共用根目录，空值回退代码默认 "trace_output"（相对启动 cwd）。
  // 绝对化：do_daemonize()（步骤 2）会 chdir("/")，若此处存相对路径，daemonize 模式下
  // 后续写 span 会解析到 "/trace_output"（非 root 用户 EACCES 被 catch 吞掉 → trace 静默
  // 丢失）；基于启动 cwd 转绝对路径后 immune 到 chdir，且 worker 注入的同值也变为绝对。
  {
    std::string trace_dir =
        config_.trace_output_dir.empty() ? "trace_output" : config_.trace_output_dir;
    std::error_code ec;
    std::filesystem::path abs_dir = std::filesystem::absolute(trace_dir, ec);
    runtime::trace::TraceManager::instance().set_output_dir(
        ec ? trace_dir : abs_dir.string());
  }

  Logger::instance().info("[DAEMON] agentflowd starting...");

  // 2. Daemonize if requested
  if (config_.daemonize) {
    if (!do_daemonize()) {
      Logger::instance().error("[DAEMON] Failed to daemonize");
      return false;
    }
  }

  // 2b. Pre-install agentflow-runtime in the default Python interpreter.
  //     This provides a fallback when the agent's .venv is unavailable or
  //     incomplete — the daemon's default Python always has the package.
  if (!config_.runtime_pkg_dir.empty()) {
    std::string cmd = "pip install -e " + config_.runtime_pkg_dir + " --quiet 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc == 0) {
      Logger::instance().info_f("[RUNTIME_PKG] Pre-installed agentflow-runtime from %s",
                                config_.runtime_pkg_dir.c_str());
    } else {
      Logger::instance().warn_f("[RUNTIME_PKG] Failed to pre-install agentflow-runtime from %s "
                                "(exit=%d), continuing without fallback",
                                config_.runtime_pkg_dir.c_str(), rc);
    }
  }

  // 3. Initialize DataRef Bridge (Bus-B) before any store_payload call
  framework::dataref_bridge::init(
      config_.dataref_bridge_config.capacity_mb,
      config_.dataref_bridge_config.redis_url);

  // 3.5 Initialize Sandbox management (AgentFoundry 沙箱机制移植，对齐 lld.md T014)
  // 注入点：DataRef Bridge init 后、Runtime 创建前——agent start 时已可持有沙箱映射
  // sandbox-mode=none 时 SandboxLifecycle 内部走 nullptr 路径（向后兼容，FR-006）
  // RpcHandler 无论是否创建 Sandbox 都要维护 user/client 会话，
  // 因此 SessionManager 不能只在启用 Sandbox 时初始化。
  session_manager_ = std::make_unique<SessionManager>();
  if (config_.sandbox_config.mode != "none") {
    // v2 段1 预热：TemplateRegistry 按 agent_id 预建 e2b template（改动 #1）
    // 仅 e2b 模式内部有效；Local/Docker 模式 preload no-op（无 template 概念）
    template_registry_ = std::make_unique<TemplateRegistry>(config_);
    sandbox_lifecycle_ = std::make_unique<SandboxLifecycle>(
        *session_manager_, config_, *template_registry_);
    sandbox_snapshots_ = std::make_unique<SandboxSnapshots>(
        *sandbox_lifecycle_, config_);
    sandbox_lifecycle_->set_snapshots(*sandbox_snapshots_);  // 反向注入，供 destroy/recycle 使用
    sandbox_lifecycle_->start_gc_thread();  // 非阻塞，GC 周期从 config.gc_destroy_ttl_ms 读取
    Logger::instance().info_f(
        "[SANDBOX] Initialized sandbox management: mode=%s, max_active=%d",
        config_.sandbox_config.mode.c_str(),
        config_.sandbox_config.sandbox_max_active);
  } else {
    Logger::instance().info(
        "[SANDBOX] sandbox-mode=none, bypass sandbox init (backward compat)");
  }

  // 4. Create Runtime
  runtime_ = std::make_unique<runtime::MMAgentRuntime>(config_.runtime_config);

  // 5. Create AgentRegistry and load configured agents
  registry_ = std::make_unique<AgentRegistry>();
  if (!load_agents()) {
    Logger::instance().error("[DAEMON] Failed to load agents");
    runtime_.reset();  // never started, safe to discard
    return false;
  }

  // 5. Start Runtime (before agents)
  if (!runtime_->start()) {
    Logger::instance().error("[DAEMON] Failed to start runtime");
    runtime_.reset();
    return false;
  }
  Logger::instance().info("[DAEMON] Runtime started");

  // 6. Start Agents (after runtime)
  auto agents = runtime_->list_agents();
  for (auto& agent : agents) {
    if (!agent->start()) {
      Logger::instance().error_f("[DAEMON] Failed to start agent: %s", agent->get_id().c_str());
      // Runtime + partial agents need cleanup
      shutdown_services();
      return false;
    }
    Logger::instance().info_f("[DAEMON] Agent started: %s (%s)", agent->get_id().c_str(),
                              agent->get_name().c_str());
  }

  // 7. Create ResponsePoller and EventBroker (before RpcHandler, which holds references)
  response_poller_ = std::make_unique<ResponsePoller>();
  event_broker_ = std::make_unique<EventBroker>();

  // 8. Create and start RPC Server
  rpc_server_ = std::make_unique<RpcServer>();
  rpc_server_->set_disconnect_handler([this](int client_fd) {
    if(response_poller_) {
      response_poller_->remove_requests_for_client(client_fd);
    }
  });
  rpc_handler_ = std::make_unique<RpcHandler>(*runtime_, *registry_, *response_poller_,
                                               *event_broker_, *session_manager_,
                                               sandbox_lifecycle_.get(),
                                               sandbox_snapshots_.get(),
                                               config_.sandbox_config.allow_fallback);
  rpc_handler_->register_all(*rpc_server_);

  RpcServer::Config rpc_config;
  rpc_config.listen = config_.listen;
  rpc_config.uds_biz_path = config_.uds_biz_path;
  rpc_config.uds_ctrl_path = config_.uds_ctrl_path;
  rpc_config.max_connections = config_.max_connections;

  if (!rpc_server_->start(rpc_config)) {
    Logger::instance().error("[DAEMON] Failed to start RPC server");
    // Runtime + agents started, need full cleanup
    shutdown_services();
    return false;
  }

  // 9. Start ResponsePoller and EventBroker (after RPC server)
  response_poller_->start(*registry_, *rpc_server_);
  event_broker_->start(*rpc_server_);

  // 10. Register EventBroker as IEventHandler with runtime
  runtime_->register_event_handler(
      std::shared_ptr<runtime::IEventHandler>(event_broker_.get(), [](auto*) {}));

  // 11. Write PID file
  if (!config_.pid_file.empty()) {
    std::ofstream pf(config_.pid_file);
    if (pf.is_open()) {
      pf << getpid() << std::endl;
    }
  }

  start_time_ms_ = now_ms();
  Logger::instance().info("[DAEMON] agentflowd initialized successfully");
  return true;
}

int Daemon::run() {
  Logger::instance().info("[DAEMON] Main loop waiting for shutdown signal...");

  while (shutdown_phase_.load() == ShutdownPhase::NONE) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  Logger::instance().info("[DAEMON] Shutdown initiated, entering drain phase...");

  // Phase 1: DRAINING
  auto drain_start = now_ms();
  while (shutdown_phase_.load() == ShutdownPhase::DRAINING) {
    bool running_nodes = runtime_ ? runtime_->get_running_nodes() > 0 : false;
    bool elapsed = (now_ms() - drain_start) >= config_.drain_timeout_ms;

    if (!running_nodes || elapsed) {
      if (elapsed) {
        Logger::instance().warn("[DAEMON] Drain timeout exceeded, forcing stop");
      }
      shutdown_phase_ = ShutdownPhase::STOPPING;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  shutdown_services();

  Logger::instance().info("[DAEMON] agentflowd shutdown complete");
  return 0;
}

void Daemon::shutdown_services() {
  // Order matters: stop accepting new work first, then drain, then tear down.

  // Stop ResponsePoller
  if (rpc_server_) {
    rpc_server_->set_disconnect_handler({});
  }
  if (response_poller_) {
    response_poller_->stop();
    Logger::instance().info("[DAEMON] ResponsePoller stopped");
    response_poller_.reset();
  }

  // Stop EventBroker
  if (event_broker_) {
    event_broker_->stop();
    Logger::instance().info("[DAEMON] EventBroker stopped");
    event_broker_.reset();
  }

  // Stop RPC server first (no new requests)
  if (rpc_server_) {
    rpc_server_->stop();
    Logger::instance().info("[DAEMON] RPC server stopped");
    rpc_server_.reset();
  }

  // Stop agents then runtime
  if (runtime_) {
    auto agents = runtime_->list_agents();
    for (auto& agent : agents) {
      agent->stop();
      Logger::instance().info_f("[DAEMON] Agent stopped: %s", agent->get_id().c_str());
    }

    runtime_->stop();
    Logger::instance().info("[DAEMON] Runtime stopped");
    runtime_.reset();
  }

  // Destroy all sandbox resources (FR-023: explicit cleanup, not relying on RAII chain)
  if (session_manager_) {
    session_manager_->destroy_all();
    Logger::instance().info("[SANDBOX] All sandboxes destroyed via explicit cleanup");
  }

  // 显式回收过期快照（FR-023）：避免依赖 RAII 析构链
  // gc_recycle 在 GC 线程停止前再跑一次，确保本次运行产生的过期快照被清理
  if (sandbox_snapshots_) {
    try {
      sandbox_snapshots_->gc_recycle();
      Logger::instance().info("[SANDBOX] Snapshot gc_recycle completed on shutdown");
    } catch (const std::exception& e) {
      Logger::instance().warn_f("[SANDBOX] Snapshot gc_recycle on shutdown failed: %s", e.what());
    }
  }

  // 清理 UDS socket / PID 文件（与 force_shutdown 共用同一实现，FR-023）
  cleanup_socket_and_pid_files();
}

void Daemon::shutdown() {
  if (shutdown_phase_.load() == ShutdownPhase::NONE) {
    shutdown_phase_ = ShutdownPhase::DRAINING;
  }
}

void Daemon::cleanup_socket_and_pid_files() {
  // 清理 UDS socket 文件（biz + ctrl）
  if (!config_.uds_biz_path.empty()) unlink(config_.uds_biz_path.c_str());
  if (!config_.uds_ctrl_path.empty()) unlink(config_.uds_ctrl_path.c_str());
  // 清理 PID 文件
  if (!config_.pid_file.empty()) unlink(config_.pid_file.c_str());
}

void Daemon::force_shutdown() {
  // FR-023: quick cleanup before _exit(1) on forced shutdown (second SIGTERM/SIGINT)
  // Destroy sandbox resources synchronously, then let _exit(1) do the rest.
  // Each LocalSandbox::destroy() takes ~1s (SIGTERM → 1s → SIGKILL → waitpid),
  // so for N sandboxes this blocks up to N seconds — acceptable for forced shutdown.
  if (session_manager_) {
    session_manager_->destroy_all();
    Logger::instance().info("[SANDBOX] All sandboxes destroyed via force_shutdown");
  }
  // 与 shutdown_services() 一致地清理 UDS socket / PID 文件
  // 防止下次启动 bind() 失败或外部监控误判 daemon 仍在运行
  cleanup_socket_and_pid_files();
}

void Daemon::cleanup_orphan_sandboxes() {
  // FR-025 L3: 启动时清理上一次 crash 残留的孤儿资源
  // 幂等：无残留则无操作
  Logger::instance().info_f("[SANDBOX] Starting orphan sandbox cleanup (mode=%s)...",
                            config_.sandbox_config.mode.c_str());

  // 1. LocalSandbox：杀 orphaned sandbox_http_server.py 进程
  //    匹配规则：命令行含 sandbox_http_server.py + ppid==1（被 init 收养）
  //    说明：覆盖 --parent-pid 命令行参数 和 DAEMON_PID 环境变量两种启动模式
  //          (前者 argv 含 --parent-pid；后者仅靠环境变量，argv 不含特征)
  //          因此按 ppid==1 判定孤儿，不依赖 --parent-pid 命令行参数
  //    macOS 兼容：用 ps -o ppid= 代替 /proc/$pid/stat
  Logger::instance().info("[SANDBOX] Orphan cleanup step 1: LocalSandbox processes...");
  int rc = std::system(
      "for pid in $(pgrep -f 'sandbox_http_server.py' 2>/dev/null); do "
      "  ppid=$(ps -o ppid= -p $pid 2>/dev/null | tr -d ' '); "
      "  [ \"$ppid\" = \"1\" ] && kill $pid 2>/dev/null; "
      "done");
  (void)rc;

  // 2. Docker：删带 label 的残留容器（仅 local/docker 模式需要）
  //    E2B 模式使用远程沙箱，本地无 Docker 容器，直接跳过
  if (config_.sandbox_config.mode == "e2b") {
    Logger::instance().info("[SANDBOX] Orphan cleanup step 2: mode=e2b, skipping Docker cleanup");
  } else {
    //    幂等：无匹配容器时 docker 输出空列表，不报错
    //    注意：std::system() 由 /bin/sh -c 执行，命令替换 $(docker ps ...)
    //         在 timeout 之前展开，docker ps 挂起时 timeout 失效。
    //    修复：用管道 | 让 timeout 直接覆盖 docker ps，避免命令替换陷阱。
    //    超时保护：3s 超时，防止 Docker daemon 不可达时阻塞 daemon 启动。
    Logger::instance().info("[SANDBOX] Orphan cleanup step 2: Docker sandbox containers...");
    int docker_rc = std::system(
        "timeout 3 docker ps -q --filter label=agentflow.sandbox=1 2>/dev/null "
        "| xargs -r timeout 3 docker rm -f 2>/dev/null");
    if (docker_rc != 0 && docker_rc != 1) {
      // 非零通常意味着超时或 docker 不可用，仅记录不阻塞
      Logger::instance().warn_f("[SANDBOX] Docker orphan cleanup returned rc=%d "
                                "(timeout/docker-unavailable is normal)", docker_rc);
    }
  }

  Logger::instance().info("[SANDBOX] Orphan cleanup completed");
}

bool Daemon::load_agents() {
  if (config_.agents.empty()) {
    Logger::instance().warn("[DAEMON] No agents configured");
    return true;
  }

  for (const auto& entry : config_.agents) {
    if (!registry_->load_agent(entry.package_path, entry.name_override, *runtime_)) {
      return false;
    }
    // v2 段1 预热：config 启动路径 load agent 也触发 template preload
    //（与 RPC handle_agent_load 一致——生产上 agent 常通过 config 启动加载，
    //  若此处不预热，config 加载的 agent 首问仍冷启动，违背 v2 设计意图）
    // 失败不拒绝启动：warning 降级，execute 首问走 v1 懒加载兜底（用户 2026-08-08 拍板）
    auto loaded = registry_->list_all();
    if (!loaded.empty() && sandbox_lifecycle_) {
      const auto& last = loaded.back();
      try {
        bool ok = sandbox_lifecycle_->preload_template(last.agent_id, last.package_path);
        if (!ok) {
          Logger::instance().warn_f(
              "[TEMPLATE] preload degraded for agent=%s (config load path, execute 首问走 v1 兜底)",
              last.agent_id.c_str());
        }
      } catch (const std::exception& e) {
        Logger::instance().warn_f(
            "[TEMPLATE] preload threw for agent=%s: %s (config load path, execute 首问走 v1 兜底)",
            last.agent_id.c_str(), e.what());
      }
    }
  }
  return true;
}

bool Daemon::do_daemonize() {
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid > 0) _exit(0);

  if (setsid() < 0) return false;

  pid = fork();
  if (pid < 0) return false;
  if (pid > 0) _exit(0);

  ::close(STDIN_FILENO);
  ::close(STDOUT_FILENO);
  ::close(STDERR_FILENO);

  int devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > 2) ::close(devnull);
  }

  chdir("/");
  return true;
}

}  // namespace daemon
}  // namespace agentflow
