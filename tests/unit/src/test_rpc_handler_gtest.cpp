// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include "daemon/rpc_server.h"

namespace agentflow {
namespace daemon {
namespace tests {

class RpcServerTest : public ::testing::Test {};

TEST_F(RpcServerTest, MethodAccessControlTCP) {
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "health"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "agent.execute"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "agent.load"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "runtime.shutdown"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "databus.put"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "event.subscribe"));
}

TEST_F(RpcServerTest, MethodAccessControlUDSBiz) {
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "agent.execute"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "agent.poll_response"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "databus.put"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "databus.get"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "event.subscribe"));

  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "agent.load"));
  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "agent.unload"));
  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "runtime.shutdown"));
  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "health"));
}

TEST_F(RpcServerTest, MethodAccessControlUDSCtrl) {
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "agent.load"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "agent.unload"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "runtime.shutdown"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "health"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "metrics.get"));

  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "agent.execute"));
  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "databus.put"));
  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "event.subscribe"));
}

TEST_F(RpcServerTest, UnknownMethodDenied) {
  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_BIZ, "nonexistent"));
  EXPECT_FALSE(RpcServer::is_method_allowed(ChannelType::UDS_CTRL, "nonexistent"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "nonexistent"));
}

}  // namespace tests
}  // namespace daemon
}  // namespace agentflow
