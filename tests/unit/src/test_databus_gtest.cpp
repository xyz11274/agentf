// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include "runtime/databus/databus.h"
#include "runtime/databus/file_databus_bridge.h"

class DataBusTest : public ::testing::Test {
protected:
  agentflow::runtime::MMDataBus databus_{1024 * 1024};

  void SetUp() override {
    databus_.connect_remote("redis://127.0.0.1:6379");
  }
};

TEST_F(DataBusTest, PutGetLocalCache) {
  ASSERT_TRUE(databus_.put("key1", std::string("value1")));

  agentflow::DataVariant value;
  ASSERT_TRUE(databus_.get("key1", value));
  ASSERT_EQ(std::get<std::string>(value), "value1");
}

TEST_F(DataBusTest, StoreResolveDataRef) {
  agentflow::runtime::MMDataBus::OwnerContext owner;
  owner.agent_id = "agent-A";
  owner.session_id = "session-1";

  agentflow::runtime::MMDataBus::DataRef ref;
  ASSERT_TRUE(databus_.store_with_ref("msg:m1", std::string("payload"), owner, ref));
  ASSERT_FALSE(ref.address.empty());

  agentflow::DataVariant resolved;
  ASSERT_TRUE(databus_.resolve_ref(ref.address, resolved));
  ASSERT_EQ(std::get<std::string>(resolved), "payload");
}

TEST_F(DataBusTest, FindRefsByOwner) {
  agentflow::runtime::MMDataBus::OwnerContext owner;
  owner.user_id = "user-42";
  owner.workflow_id = "wf-main";

  agentflow::runtime::MMDataBus::DataRef ref;
  ASSERT_TRUE(databus_.store_with_ref("msg:owner", std::string("owner-payload"), owner, ref));

  auto refs = databus_.find_refs_by_owner("user_id", "user-42");
  ASSERT_FALSE(refs.empty());
}

TEST_F(DataBusTest, FileBridgeSupportsSetGetAndPublish) {
  auto root = agentflow::runtime::FileDataBusBridge::make_temp_root();
  agentflow::runtime::FileDataBusBridge bridge(root);
  ASSERT_TRUE(bridge.initialize());
  ASSERT_TRUE(bridge.put("session.message", std::string("hello")));

  agentflow::DataVariant value;
  ASSERT_TRUE(bridge.get("session.message", value));
  ASSERT_EQ(std::get<std::string>(value), "hello");

  ASSERT_TRUE(bridge.publish("stream.chunk", std::string("chunk-1")));
  std::set<std::string> seen;
  auto                  events = bridge.poll_events(seen);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().topic, "stream.chunk");
  EXPECT_EQ(std::get<std::string>(events.back().value), "chunk-1");

  bridge.cleanup();
}
