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

  ShutdownPhase get_shutdown_phase() const { return shutdown_phase_.load(); }

private:
  bool load_agents();
  bool do_daemonize();

  DaemonConfig config_;
  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::unique_ptr<AgentRegistry> registry_;
  std::unique_ptr<RpcServer> rpc_server_;
  std::unique_ptr<RpcHandler> rpc_handler_;
  std::unique_ptr<ResponsePoller> response_poller_;
  std::unique_ptr<EventBroker> event_broker_;
  std::atomic<ShutdownPhase> shutdown_phase_{ShutdownPhase::NONE};
  int64_t start_time_ms_ = 0;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_DAEMON_H
