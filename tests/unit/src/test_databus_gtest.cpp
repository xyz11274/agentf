// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include "runtime/databus/databus.h"

class DataBusTest : public ::testing::Test {
protected:
  agentflow::runtime::DataBus databus_{1024 * 1024};

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
  agentflow::runtime::DataBus::OwnerContext owner;
  owner.agent_id = "agent-A";
  owner.session_id = "session-1";

  agentflow::runtime::DataBus::DataRef ref;
  ASSERT_TRUE(databus_.store_with_ref("msg:m1", std::string("payload"), owner, ref));
  ASSERT_FALSE(ref.address.empty());

  agentflow::DataVariant resolved;
  ASSERT_TRUE(databus_.resolve_ref(ref.address, resolved));
  ASSERT_EQ(std::get<std::string>(resolved), "payload");
}

TEST_F(DataBusTest, FindRefsByOwner) {
  agentflow::runtime::DataBus::OwnerContext owner;
  owner.user_id = "user-42";
  owner.workflow_id = "wf-main";

  agentflow::runtime::DataBus::DataRef ref;
  ASSERT_TRUE(databus_.store_with_ref("msg:owner", std::string("owner-payload"), owner, ref));

  auto refs = databus_.find_refs_by_owner("user_id", "user-42");
  ASSERT_FALSE(refs.empty());
}
