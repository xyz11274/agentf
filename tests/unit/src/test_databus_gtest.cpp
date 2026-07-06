// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include "agent/data_variant_utils.h"
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


TEST_F(DataBusTest, FileDescriptorRoundTripsThroughRefsAndFileBridge) {
  agentflow::FileDescriptor descriptor;
  descriptor.file_id = "file-123";
  descriptor.uri = "file:///tmp/audio.wav";
  descriptor.storage_backend = "filesystem";
  descriptor.media_type = "audio/wav";
  descriptor.size_bytes = 4096;
  descriptor.checksum = "sha256:abc";
  descriptor.created_at = 1700000000;
  descriptor.ttl_seconds = 3600;
  descriptor.owner = "session-1";
  descriptor.metadata_json = R"({"lang":"zh","duration_ms":1200})";

  ASSERT_TRUE(databus_.put("asset.single", descriptor));
  agentflow::DataVariant fetched;
  ASSERT_TRUE(databus_.get("asset.single", fetched));
  ASSERT_TRUE(std::holds_alternative<agentflow::FileDescriptor>(fetched));
  EXPECT_EQ(std::get<agentflow::FileDescriptor>(fetched), descriptor);

  agentflow::runtime::MMDataBus::OwnerContext owner;
  owner.session_id = "session-1";
  agentflow::runtime::MMDataBus::DataRef ref;
  ASSERT_TRUE(databus_.store_with_ref("asset.ref", descriptor, owner, ref));

  agentflow::DataVariant resolved;
  ASSERT_TRUE(databus_.resolve_ref(ref.address, resolved));
  EXPECT_EQ(std::get<agentflow::FileDescriptor>(resolved), descriptor);

  auto root = agentflow::runtime::FileDataBusBridge::make_temp_root();
  agentflow::runtime::FileDataBusBridge bridge(root);
  ASSERT_TRUE(bridge.initialize());
  ASSERT_TRUE(bridge.put("asset.single", descriptor));

  agentflow::DataVariant bridged;
  ASSERT_TRUE(bridge.get("asset.single", bridged));
  ASSERT_TRUE(std::holds_alternative<agentflow::FileDescriptor>(bridged));
  EXPECT_EQ(std::get<agentflow::FileDescriptor>(bridged), descriptor);

  agentflow::FileDescriptorList files{descriptor, descriptor};
  ASSERT_TRUE(bridge.publish("asset.batch", files));
  std::set<std::string> seen_events;
  auto events = bridge.poll_events(seen_events);
  ASSERT_FALSE(events.empty());
  ASSERT_TRUE(std::holds_alternative<agentflow::FileDescriptorList>(events.back().value));
  EXPECT_EQ(std::get<agentflow::FileDescriptorList>(events.back().value), files);

  bridge.cleanup();
}

// ── parse_scalar_json_object tests ─────────────────────────────

TEST(ParseScalarJson, EmptyObject) {
  std::map<std::string, std::string> out;
  EXPECT_TRUE(agentflow::runtime::FileDataBusBridge::parse_scalar_json_object("{}", out));
  EXPECT_TRUE(out.empty());
}

TEST(ParseScalarJson, SimpleStringValues) {
  std::map<std::string, std::string> out;
  EXPECT_TRUE(agentflow::runtime::FileDataBusBridge::parse_scalar_json_object(
      R"({"name":"Alice","role":"admin"})", out));
  EXPECT_EQ(out["name"], "Alice");
  EXPECT_EQ(out["role"], "admin");
}

TEST(ParseScalarJson, NumericBoolNullValues) {
  std::map<std::string, std::string> out;
  EXPECT_TRUE(agentflow::runtime::FileDataBusBridge::parse_scalar_json_object(
      R"({"count":42,"ratio":3.14,"active":true,"nothing":null})", out));
  EXPECT_EQ(out["count"], "42");
  // std::to_string formats double as "3.140000"
  EXPECT_EQ(out["ratio"], "3.140000");
  EXPECT_EQ(out["active"], "true");
  EXPECT_EQ(out["nothing"], "null");
}

TEST(ParseScalarJson, EscapedAndUnicodeValues) {
  std::map<std::string, std::string> out;
  EXPECT_TRUE(agentflow::runtime::FileDataBusBridge::parse_scalar_json_object(
      R"({"msg":"say \"hello\"","name":"中文测试"})", out));
  EXPECT_EQ(out["msg"], "say \"hello\"");
  EXPECT_EQ(out["name"], "中文测试");
}

TEST(ParseScalarJson, MultipleEntries) {
  std::map<std::string, std::string> out;
  EXPECT_TRUE(agentflow::runtime::FileDataBusBridge::parse_scalar_json_object(
      R"({"a":"1","b":"2","c":"3","d":"4","e":"5"})", out));
  EXPECT_EQ(out.size(), 5u);
  EXPECT_EQ(out["e"], "5");
}

TEST(ParseScalarJson, NestedObjectReturnsFalse) {
  std::map<std::string, std::string> out;
  EXPECT_FALSE(agentflow::runtime::FileDataBusBridge::parse_scalar_json_object(
      R"({"k":{"nested":"v"}})", out));
}

TEST(ParseScalarJson, InvalidJsonReturnsFalse) {
  std::map<std::string, std::string> out;
  EXPECT_FALSE(agentflow::runtime::FileDataBusBridge::parse_scalar_json_object(
      "{invalid", out));
}

// ── DataBus capacity tracking — overwrite same key ──────────────────

TEST_F(DataBusTest, OverwriteSameKeyTracksCapacityCorrectly) {
  // Write a key once
  ASSERT_TRUE(databus_.put("k1", std::string("hello")));
  size_t after_first = databus_.get_used_capacity();

  // Overwrite with a LARGER value — capacity should grow by the delta
  std::string bigger(1000, 'x');
  ASSERT_TRUE(databus_.put("k1", bigger));
  size_t after_bigger = databus_.get_used_capacity();
  EXPECT_GT(after_bigger, after_first);
  EXPECT_LT(after_bigger, after_first + 2000);  // sanity: not 10x overhead

  // Overwrite with a SMALLER value — capacity should shrink
  ASSERT_TRUE(databus_.put("k1", std::string("hi")));
  size_t after_smaller = databus_.get_used_capacity();
  EXPECT_LT(after_smaller, after_bigger);

  // Verify data is correct after overwrites
  agentflow::DataVariant val;
  ASSERT_TRUE(databus_.get("k1", val));
  EXPECT_EQ(std::get<std::string>(val), "hi");

  // Remove and verify capacity drops
  ASSERT_TRUE(databus_.remove("k1"));
  EXPECT_EQ(databus_.get_data_count(), 0);
}
