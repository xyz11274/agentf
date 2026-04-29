// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "daemon/config.h"

namespace agentflow {
namespace daemon {
namespace tests {

class DaemonConfigTest : public ::testing::Test {
protected:
  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() / "af_config_test";
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(tmp_dir_);
  }

  std::string write_config(const std::string& content) {
    auto path = tmp_dir_ / "test_config.json";
    std::ofstream f(path);
    f << content;
    return path.string();
  }

  std::filesystem::path tmp_dir_;
};

TEST_F(DaemonConfigTest, DefaultConfig) {
  DaemonConfig config;
  EXPECT_EQ(config.listen, "0.0.0.0:7890");
  EXPECT_EQ(config.uds_biz_path, "/tmp/agentflowd_biz.sock");
  EXPECT_EQ(config.uds_ctrl_path, "/tmp/agentflowd_ctrl.sock");
  EXPECT_EQ(config.max_connections, 1024);
  EXPECT_EQ(config.daemonize, false);
  EXPECT_TRUE(config.pid_file.empty());
}

TEST_F(DaemonConfigTest, LoadConfigFile) {
  std::string config_path = write_config(
      R"({
        "server": {
          "listen": "127.0.0.1:9999",
          "uds_biz_path": "/tmp/biz.sock",
          "uds_ctrl_path": "/tmp/ctrl.sock",
          "max_connections": 512
        },
        "logging": {
          "level": "debug"
        },
        "daemon": {
          "drain_timeout_ms": 10000
        }
      })");

  DaemonConfig config;
  config.config_file = config_path;
  EXPECT_TRUE(load_config_file(config));
  EXPECT_EQ(config.listen, "127.0.0.1:9999");
  EXPECT_EQ(config.uds_biz_path, "/tmp/biz.sock");
  EXPECT_EQ(config.uds_ctrl_path, "/tmp/ctrl.sock");
  EXPECT_EQ(config.max_connections, 512);
  EXPECT_EQ(config.log_level, "debug");
  EXPECT_EQ(config.drain_timeout_ms, 10000);
}

TEST_F(DaemonConfigTest, LoadConfigFileMissing) {
  DaemonConfig config;
  config.config_file = "/nonexistent/config.json";
  EXPECT_FALSE(load_config_file(config));
}

TEST_F(DaemonConfigTest, LoadConfigFilePartial) {
  std::string config_path = write_config(
      R"({
        "server": {
          "listen": "0.0.0.0:8080"
        }
      })");

  DaemonConfig config;
  config.config_file = config_path;
  EXPECT_TRUE(load_config_file(config));
  EXPECT_EQ(config.listen, "0.0.0.0:8080");
  EXPECT_EQ(config.uds_biz_path, "/tmp/agentflowd_biz.sock");
}

TEST_F(DaemonConfigTest, ValidateConfig) {
  DaemonConfig config;
  config.listen = "0.0.0.0:7890";
  EXPECT_TRUE(validate_config(config));

  config.listen = "";
  EXPECT_FALSE(validate_config(config));

  config.listen = "0.0.0.0:7890";
  config.max_connections = 0;
  EXPECT_FALSE(validate_config(config));
}

}  // namespace tests
}  // namespace daemon
}  // namespace agentflow
