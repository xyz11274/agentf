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
#include "agent/package.h"
#include "daemon/agent_registry.h"  // NOLINT
#include "daemon/rpc_handler.h"     // NOLINT
#include "daemon/rpc_server.h"      // NOLINT
#include "daemon/response_poller.h" // NOLINT
#include "daemon/event_broker.h"    // NOLINT // NOLINT
#include "runtime/agent_runtime.h"
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
}

bool Daemon::initialize() {
  g_daemon = this;
  install_signal_handlers();

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
  }
  Logger::instance().info("[DAEMON] agentflowd starting...");

  // 2. Daemonize if requested
  if (config_.daemonize) {
    if (!do_daemonize()) {
      Logger::instance().error("[DAEMON] Failed to daemonize");
      return false;
    }
  }

  // 3. Create Runtime
  runtime_ = std::make_unique<runtime::MMAgentRuntime>(config_.runtime_config);

  // 4. Create AgentRegistry and load configured agents
  registry_ = std::make_unique<AgentRegistry>();
  if (!load_agents()) {
    Logger::instance().error("[DAEMON] Failed to load agents");
    return false;
  }

  // 5. Start Runtime (before agents)
  if (!runtime_->start()) {
    Logger::instance().error("[DAEMON] Failed to start runtime");
    return false;
  }
  Logger::instance().info("[DAEMON] Runtime started");

  // 6. Start Agents (after runtime)
  auto agents = runtime_->list_agents();
  for (auto& agent : agents) {
    if (!agent->start()) {
      Logger::instance().error_f("[DAEMON] Failed to start agent: %s", agent->get_id().c_str());
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
  rpc_handler_ = std::make_unique<RpcHandler>(*runtime_, *registry_, *response_poller_, *event_broker_);
  rpc_handler_->register_all(*rpc_server_);

  RpcServer::Config rpc_config;
  rpc_config.listen = config_.listen;
  rpc_config.uds_biz_path = config_.uds_biz_path;
  rpc_config.uds_ctrl_path = config_.uds_ctrl_path;
  rpc_config.max_connections = config_.max_connections;

  if (!rpc_server_->start(rpc_config)) {
    Logger::instance().error("[DAEMON] Failed to start RPC server");
    return false;
  }

  // 9. Start ResponsePoller and EventBroker
  response_poller_->start(*registry_, *rpc_server_);
  event_broker_->start(*rpc_server_);

  // 10. Register EventBroker as IEventHandler with runtime
  runtime_->register_event_handler(
      std::shared_ptr<runtime::IEventHandler>(event_broker_.get(), [](auto*) {}));

  // 8. Write PID file
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

  // Phase 2: STOPPING
  // Stop ResponsePoller
  if (response_poller_) {
    response_poller_->stop();
    Logger::instance().info("[DAEMON] ResponsePoller stopped");
  }

  // Stop EventBroker
  if (event_broker_) {
    event_broker_->stop();
    Logger::instance().info("[DAEMON] EventBroker stopped");
  }

  // Stop RPC server first (no new requests)
  if (rpc_server_) {
    rpc_server_->stop();
    Logger::instance().info("[DAEMON] RPC server stopped");
  }

  if (runtime_) {
    auto agents = runtime_->list_agents();
    for (auto& agent : agents) {
      agent->stop();
      Logger::instance().info_f("[DAEMON] Agent stopped: %s", agent->get_id().c_str());
    }

    runtime_->stop();
    Logger::instance().info("[DAEMON] Runtime stopped");
  }

  // Clean up UDS socket files
  if (!config_.uds_biz_path.empty()) unlink(config_.uds_biz_path.c_str());
  if (!config_.uds_ctrl_path.empty()) unlink(config_.uds_ctrl_path.c_str());

  // Remove PID file
  if (!config_.pid_file.empty()) unlink(config_.pid_file.c_str());

  Logger::instance().info("[DAEMON] agentflowd shutdown complete");
  return 0;
}

void Daemon::shutdown() {
  if (shutdown_phase_.load() == ShutdownPhase::NONE) {
    shutdown_phase_ = ShutdownPhase::DRAINING;
  }
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
