// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// agentflowd - AgentFlow daemon entry point
// Standalone executable that runs AgentFlow as a service with JSON-RPC 2.0 interface

#include <cstdlib>
#include <iostream>
#include <system_error>

#include "daemon/config.h"   // NOLINT
#include "daemon/daemon.h"   // NOLINT

using namespace agentflow::daemon;

int main(int argc, char* argv[]) {
  DaemonConfig config;

  // Step 1: Parse CLI arguments
  if (!parse_cli_args(argc, argv, config)) {
    return 0;  // --help or --version was handled
  }

  // Step 2: Apply config file + env overrides (CLI > ENV > File > Default)
  if (!merge_config(config)) {
    std::cerr << "Error: failed to merge configuration" << std::endl;
    return 1;
  }

  // Step 3: Validate
  if (!validate_config(config)) {
    return 1;
  }

  // Step 4: If --validate, check package structures and exit
  if (config.validate) {
    std::cout << "[OK] Configuration valid" << std::endl;
    std::cout << "  listen: " << config.listen << std::endl;
    std::cout << "  uds_biz_path: " << config.uds_biz_path << std::endl;
    std::cout << "  uds_ctrl_path: " << config.uds_ctrl_path << std::endl;
    std::cout << "  agents: " << config.agents.size() << std::endl;
    for (const auto& a : config.agents) {
      std::cout << "    - " << a.package_path << std::endl;
    }
    return 0;
  }

  // Step 5: Create and initialize Daemon
  //
  // Wrap in try-catch to prevent uncaught exceptions (e.g.
  // std::system_error from mutex operations during static destruction)
  // from terminating the process — a known issue on macOS/libc++
  // when daemon fails to initialize after partial runtime start
  // (F-20260704-001).
  try {
    Daemon daemon(config);
    if (!daemon.initialize()) {
      std::cerr << "Error: failed to initialize agentflowd" << std::endl;
      return 1;
    }

    // Step 6: Run (blocks until shutdown)
    return daemon.run();
  } catch (const std::system_error& e) {
    std::cerr << "Fatal: system error in agentflowd lifecycle: " << e.what()
              << " (code=" << e.code() << ")" << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Fatal: unhandled exception in agentflowd lifecycle: "
              << e.what() << std::endl;
    return 1;
  }
}
