// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include <rapidjson/document.h>

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
  // E1/E2：沙箱管理 RPC（sandbox.list / sandbox.destroy）经 TCP/WebSocket 通道（bridge 直连）
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "sandbox.list"));
  EXPECT_TRUE(RpcServer::is_method_allowed(ChannelType::TCP, "sandbox.destroy"));
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

TEST_F(RpcServerTest, ParamsPreserveQuotedDelimitersAndNestedValues) {
  char directory[] = "/tmp/af-rpc-params-XXXXXX";
  ASSERT_NE(mkdtemp(directory), nullptr);
  struct DirectoryCleanup {
    const char* path;
    ~DirectoryCleanup() { rmdir(path); }
  } cleanup{directory};
  RpcServer server;
  RpcServer::Config config;
  config.listen = "127.0.0.1:0";
  config.uds_biz_path = std::string(directory) + "/biz.sock";
  config.uds_ctrl_path = std::string(directory) + "/ctrl.sock";
  server.register_handler("agent.execute", [](const std::string& params, int64_t id, int, ChannelType) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"result\":" + (params.empty() ? "null" : params) + "}";
  });
  ASSERT_TRUE(server.start(config));
  struct SocketCleanup {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ~SocketCleanup() { if (fd >= 0) close(fd); }
  } client;
  ASSERT_GE(client.fd, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, config.uds_biz_path.c_str(), sizeof(address.sun_path) - 1);
  int connected = -1;
  for (int attempt = 0; attempt < 100 && connected != 0; ++attempt) {
    connected = connect(client.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected != 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(connected, 0);
  timeval timeout{2, 0};
  ASSERT_EQ(setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
  for (const std::string params : {
      R"({"content":"layers:2,} -> layers:2}","dialogue_id":"turn-2"})",
      R"({"content":"unmatched { and [ inside strings","nested":{"items":[1,true,null]}})",
      R"({"content":"escaped quote: \" and slash: \\ and }","params":{"content":"inner"}})",
      R"(["unmatched ]",{"value":"["},false,42])", "{}", "[]", "null", "42"}) {
    SCOPED_TRACE(params);
    const auto request = "{\"jsonrpc\":\"2.0\",\"method\":\"agent.execute\",\"id\":7,\"params\":" + params + "}\n";
    ASSERT_EQ(send(client.fd, request.data(), request.size(), MSG_NOSIGNAL), static_cast<ssize_t>(request.size()));
    std::string response;
    char byte;
    while (response.size() < 8192) {
      ASSERT_EQ(recv(client.fd, &byte, 1, 0), 1);
      if (byte == '\n') break;
      response += byte;
    }
    rapidjson::Document expected, actual;
    expected.Parse(params.c_str());
    actual.Parse(response.c_str());
    ASSERT_FALSE(actual.HasParseError());
    ASSERT_TRUE(actual.IsObject() && actual.HasMember("result"));
    if (expected.IsObject() || expected.IsArray()) EXPECT_TRUE(actual["result"] == expected);
    else EXPECT_TRUE(actual["result"].IsNull());
  }
}

}  // namespace tests
}  // namespace daemon
}  // namespace agentflow
