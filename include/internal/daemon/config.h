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
  std::string log_file;  // empty = stderr

  // Daemon
  std::string pid_file;
  bool daemonize = false;
  int drain_timeout_ms = 30000;

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
