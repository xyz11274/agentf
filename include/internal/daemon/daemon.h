// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Daemon - agentflowd orchestrator: init, startup, shutdown coordination

#ifndef AGENTFLOW_DAEMON_DAEMON_H
#define AGENTFLOW_DAEMON_DAEMON_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "daemon/config.h"

namespace agentflow {

namespace framework {
class MMAgent;
}

namespace runtime {
class MMAgentRuntime;
}

namespace daemon {

class RpcServer;
class RpcHandler;
class AgentRegistry;
class ResponsePoller;
class EventBroker;
class SessionManager;          // AgentFoundry 沙箱机制移植（lld.md T014）
class SandboxLifecycle;
class SandboxSnapshots;

enum class ShutdownPhase {
  NONE = 0,
  DRAINING = 1,
  STOPPING = 2,
  FORCED = 3,
};

class Daemon {
public:
  explicit Daemon(const DaemonConfig& config);
  ~Daemon();

  bool initialize();
  int run();
  void shutdown();
  void force_shutdown();  // FR-023: quick cleanup before _exit(1) on forced shutdown

  // FR-025 L3: cleanup orphan resources from previous crash on startup
  void cleanup_orphan_sandboxes();

  ShutdownPhase get_shutdown_phase() const { return shutdown_phase_.load(); }

private:
  bool load_agents();
  bool do_daemonize();

  // Tear down services in the correct order (reverse of initialization).
  // Called from run(), initialize() on failure, and ~Daemon().
  void shutdown_services();

  // 清理 UDS socket / PID 文件（FR-023）
  // shutdown_services() 与 force_shutdown() 共用，确保 forced 路径也清理文件
  // 不依赖 RAII 析构链，避免下次启动 bind() 失败或外部监控误判 daemon 仍在运行
  void cleanup_socket_and_pid_files();

  DaemonConfig config_;
  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::unique_ptr<AgentRegistry> registry_;
  std::unique_ptr<RpcServer> rpc_server_;
  std::unique_ptr<RpcHandler> rpc_handler_;
  std::unique_ptr<ResponsePoller> response_poller_;
  std::unique_ptr<EventBroker> event_broker_;
  // Sandbox management (AgentFoundry 沙箱机制移植，对齐 lld.md T014)
  // sandbox-mode=none 时三者均为 nullptr（向后兼容，FR-006）
  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<SandboxLifecycle> sandbox_lifecycle_;
  std::unique_ptr<SandboxSnapshots> sandbox_snapshots_;
  std::atomic<ShutdownPhase> shutdown_phase_{ShutdownPhase::NONE};
  int64_t start_time_ms_ = 0;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_DAEMON_H
