// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include <filesystem>

#include "daemon/agent_registry.h"
#include "runtime/agent_runtime.h"

namespace agentflow {
namespace daemon {
namespace tests {

class AgentRegistryTest : public ::testing::Test {
protected:
  void SetUp() override {
    runtime::MMAgentRuntime::Config config;
    config.role = runtime::MMAgentRuntime::RuntimeRole::MASTER;
    runtime_ = std::make_unique<runtime::MMAgentRuntime>(config);
    runtime_->start();
    registry_ = std::make_unique<AgentRegistry>();
  }

  void TearDown() override {
    registry_.reset();
    runtime_->stop();
    runtime_.reset();
  }

  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::unique_ptr<AgentRegistry> registry_;
};

TEST_F(AgentRegistryTest, ListAllEmpty) {
  auto entries = registry_->list_all();
  EXPECT_TRUE(entries.empty());
}

TEST_F(AgentRegistryTest, FindNonexistent) {
  auto agent = registry_->find("nonexistent");
  EXPECT_EQ(agent, nullptr);
}

TEST_F(AgentRegistryTest, LoadAgentInvalidPath) {
  EXPECT_FALSE(registry_->load_agent("/nonexistent/path", "", *runtime_));
}

TEST_F(AgentRegistryTest, UnloadNonexistent) {
  EXPECT_FALSE(registry_->unload_agent("nonexistent", *runtime_));
}

TEST_F(AgentRegistryTest, LoadAndListAgent) {
  std::string pkg_path = EXAMPLES_DIR "/agent/basic_agent";
  if (!std::filesystem::exists(pkg_path + "/package.json")) {
    GTEST_SKIP() << "basic_agent package not found";
  }

  EXPECT_TRUE(registry_->load_agent(pkg_path, "", *runtime_));

  auto entries = registry_->list_all();
  EXPECT_EQ(entries.size(), 1u);

  auto agent = registry_->find(entries[0].agent_id);
  EXPECT_NE(agent, nullptr);
}

TEST_F(AgentRegistryTest, LoadAndUnloadAgent) {
  std::string pkg_path = EXAMPLES_DIR "/agent/basic_agent";
  if (!std::filesystem::exists(pkg_path + "/package.json")) {
    GTEST_SKIP() << "basic_agent package not found";
  }

  EXPECT_TRUE(registry_->load_agent(pkg_path, "", *runtime_));

  auto entries = registry_->list_all();
  ASSERT_FALSE(entries.empty());

  EXPECT_TRUE(registry_->unload_agent(entries[0].agent_id, *runtime_));
  EXPECT_TRUE(registry_->list_all().empty());
}

}  // namespace tests
}  // namespace daemon
}  // namespace agentflow
