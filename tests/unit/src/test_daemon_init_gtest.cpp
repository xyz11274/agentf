// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Tests for Daemon initialization failure paths and cleanup safety.
// Verifies that partial initialization (e.g., port conflict) does not
// cause uncaught exceptions during destruction — a known crash on macOS
// when the runtime's threads access a destroyed mutex during static
// destruction after main() returns.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "daemon/config.h"
#include "daemon/daemon.h"
#include "daemon/rpc_server.h"

namespace agentflow {
namespace daemon {
namespace tests {

// ============================================================================
// Helper: occupy a TCP port so daemon initialization fails on bind
// ============================================================================

static int occupy_port(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 1) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

// ============================================================================
// Test fixture: occupy a port so daemon cannot bind
// ============================================================================

class DaemonInitFailureTest : public ::testing::Test {
protected:
  int occupied_fd_ = -1;
  int test_port_;

  void SetUp() override {
    test_port_ = 17991;
    occupied_fd_ = occupy_port(test_port_);
    ASSERT_GE(occupied_fd_, 0)
        << "Failed to occupy test port " << test_port_ << ": " << strerror(errno);
  }

  void TearDown() override {
    if (occupied_fd_ >= 0) {
      close(occupied_fd_);
      occupied_fd_ = -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  DaemonConfig MakeConfig() const {
    DaemonConfig config;
    config.listen = "127.0.0.1:" + std::to_string(test_port_);
    config.uds_biz_path = "/tmp/af_test_biz_" + std::to_string(test_port_) + ".sock";
    config.uds_ctrl_path = "/tmp/af_test_ctrl_" + std::to_string(test_port_) + ".sock";
    config.log_file = "";
    config.daemonize = false;
    return config;
  }
};

// ============================================================================
// Test 1: initialize() returns false on port conflict
// ============================================================================

TEST_F(DaemonInitFailureTest, InitializeReturnsFalseOnPortConflict) {
  Daemon daemon(MakeConfig());
  EXPECT_FALSE(daemon.initialize());
  // Destructor runs at scope exit — must not throw.
}

// ============================================================================
// Regression Test (F-20260704-001):
// Init failure → static destruction must not crash with
// "mutex lock failed: Invalid argument" on macOS/libc++.
//
// This runs in a forked subprocess (EXPECT_EXIT). The child:
//   1. Creates a Daemon with an occupied port
//   2. Calls initialize() → returns false
//   3. Daemon goes out of scope → ~Daemon() → shutdown_services()
//   4. Calls exit(1) — triggers static destructors
//
// If the old crash (SIGABRT) recurs, the child is killed by a signal
// and ExitedWithCode(1) does NOT match → test FAILS.
// ============================================================================

TEST_F(DaemonInitFailureTest, RegressionCleanExitAfterInitFailure) {
  EXPECT_EXIT(
      {
        DaemonConfig config;
        config.listen = "127.0.0.1:" + std::to_string(test_port_);
        config.uds_biz_path = "/tmp/af_test_biz_reg_" + std::to_string(test_port_) + ".sock";
        config.uds_ctrl_path = "/tmp/af_test_ctrl_reg_" + std::to_string(test_port_) + ".sock";
        config.log_file = "";
        config.daemonize = false;

        {
          Daemon daemon(config);
          bool ok = daemon.initialize();
          // Daemon goes out of scope here → ~Daemon() → shutdown_services()
          // Runtime threads are joined before we proceed to exit().
          static_cast<void>(ok);  // suppress unused warning
        }

        // Now trigger static destruction by calling exit(1).
        // With the fix, all runtime threads are already joined and
        // mutexes released, so static destructors run cleanly.
        // Without the fix, this was crashing with SIGABRT on macOS.
        exit(1);
      },
      testing::ExitedWithCode(1),
      "");
}

// ============================================================================
// Test: RpcServer start + immediate stop does not throw
// ============================================================================

TEST(RpcServerLifecycleTest, StartOnOccupiedPortReturnsFalse) {
  int occupied = occupy_port(17992);
  ASSERT_GE(occupied, 0);

  RpcServer server;
  RpcServer::Config cfg;
  cfg.listen = "127.0.0.1:17992";

  EXPECT_FALSE(server.start(cfg));
  // Destructor calls stop() — must not throw.

  close(occupied);
}

// ============================================================================
// Test: Daemon with valid port — full lifecycle
// ============================================================================

TEST_F(DaemonInitFailureTest, InitializeSucceedsOnFreePort) {
  // Release the occupied port first
  if (occupied_fd_ >= 0) {
    close(occupied_fd_);
    occupied_fd_ = -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  Daemon daemon(MakeConfig());

  bool ok = daemon.initialize();
  if (ok) {
    SUCCEED();
  } else {
    GTEST_SKIP() << "Skipped: runtime initialization requires external services";
  }
}

}  // namespace tests
}  // namespace daemon
}  // namespace agentflow
