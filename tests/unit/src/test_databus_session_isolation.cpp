// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Unit tests for NamespaceClient and NodeDataBusClient

#include <gtest/gtest.h>
#include <memory>
#include <regex>

#include "agentflow/so_node.h"
#include "runtime/databus/namespace_client.h"
#include "runtime/databus/node_databus_client.h"
#include "runtime/databus/node_databus_adapter.h"
#include "runtime/databus/databus.h"
#include "agent/session.h"

namespace agentflow {
namespace runtime {
namespace {

// Use the actual MMDataBus which provides all needed functionality

}  // anonymous namespace

// Test UUID generation
TEST(SessionUuidTest, GenerateUuid) {
  std::string uuid1 = framework::generate_session_uuid();
  std::string uuid2 = framework::generate_session_uuid();

  // UUIDs should be different
  EXPECT_NE(uuid1, uuid2);

  // UUID should match format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
  std::regex uuid_pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  EXPECT_TRUE(std::regex_match(uuid1, uuid_pattern)) << "UUID1: " << uuid1;
  EXPECT_TRUE(std::regex_match(uuid2, uuid_pattern)) << "UUID2: " << uuid2;

  // UUID version 4 should have '4' in the 15th position (after removing dashes, position 14)
  // Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  EXPECT_EQ(uuid1[14], '4');
  EXPECT_EQ(uuid2[14], '4');

  // Variant bits: 8, 9, a, or b in position 19
  EXPECT_TRUE(uuid1[19] == '8' || uuid1[19] == '9' || uuid1[19] == 'a' || uuid1[19] == 'b');
  EXPECT_TRUE(uuid2[19] == '8' || uuid2[19] == '9' || uuid2[19] == 'a' || uuid2[19] == 'b');
}

// Test NamespaceClientImpl key prefix building
TEST(NamespaceClientTest, KeyPrefixBuilding) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  std::string session_id = "test-session-123";
  NodeDataBusClientImpl client(&adapter, session_id);

  // Access user namespace
  auto* user_client = client.user();
  ASSERT_NE(user_client, nullptr);

  // Put a value
  SoDataVariant value(std::string("test_value"));
  EXPECT_TRUE(user_client->put("my_key", value));

  // Verify the full key was stored with session prefix
  SoDataVariant retrieved;
  EXPECT_TRUE(user_client->get("my_key", retrieved));
  EXPECT_TRUE(std::holds_alternative<std::string>(retrieved));
  EXPECT_EQ(std::get<std::string>(retrieved), "test_value");

  // Verify the raw key is stored with prefix
  SoDataVariant raw_value;
  EXPECT_TRUE(adapter.get("/session/test-session-123/user/my_key", raw_value));
}

// Test different namespaces
TEST(NamespaceClientTest, DifferentNamespaces) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  std::string session_id = "session-abc";
  NodeDataBusClientImpl client(&adapter, session_id);

  // Store in different namespaces
  client.sys()->put("config", SoDataVariant(std::string("sys_config")));
  client.user()->put("data", SoDataVariant(std::string("user_data")));
  client.llm()->put("prompt", SoDataVariant(std::string("llm_prompt")));

  // Verify they are stored separately
  SoDataVariant value;
  EXPECT_TRUE(client.sys()->get("config", value));
  EXPECT_EQ(std::get<std::string>(value), "sys_config");

  EXPECT_TRUE(client.user()->get("data", value));
  EXPECT_EQ(std::get<std::string>(value), "user_data");

  EXPECT_TRUE(client.llm()->get("prompt", value));
  EXPECT_EQ(std::get<std::string>(value), "llm_prompt");

  // Verify namespace isolation
  EXPECT_FALSE(client.sys()->contains("data"));
  EXPECT_FALSE(client.user()->contains("config"));
}

// Test session isolation
TEST(NodeDataBusClientTest, SessionIsolation) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  // Create two clients with different session IDs
  NodeDataBusClientImpl client1(&adapter, "session-1");
  NodeDataBusClientImpl client2(&adapter, "session-2");

  // Store data in each session
  client1.user()->put("shared_key", SoDataVariant(std::string("value_from_session_1")));
  client2.user()->put("shared_key", SoDataVariant(std::string("value_from_session_2")));

  // Verify isolation
  SoDataVariant value1, value2;
  EXPECT_TRUE(client1.user()->get("shared_key", value1));
  EXPECT_EQ(std::get<std::string>(value1), "value_from_session_1");

  EXPECT_TRUE(client2.user()->get("shared_key", value2));
  EXPECT_EQ(std::get<std::string>(value2), "value_from_session_2");
}

// Test session_id accessor
TEST(NodeDataBusClientTest, SessionIdAccessor) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  std::string session_id = "my-unique-session-id";
  NodeDataBusClientImpl client(&adapter, session_id);

  EXPECT_EQ(client.session_id(), session_id);
}

// Test raw() access
TEST(NodeDataBusClientTest, RawAccess) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  NodeDataBusClientImpl client(&adapter, "session-raw");

  // Write to global namespace via raw()
  INodeDataBus* raw = client.raw();
  ASSERT_NE(raw, nullptr);

  raw->put("global_key", SoDataVariant(std::string("global_value")));

  // Verify raw key is not prefixed
  SoDataVariant value;
  EXPECT_TRUE(raw->get("global_key", value));
  EXPECT_EQ(std::get<std::string>(value), "global_value");

  // Verify session-prefixed keys don't interfere
  EXPECT_FALSE(raw->contains("global_key_session-raw"));
}

// Test contains and remove operations
TEST(NamespaceClientTest, ContainsAndRemove) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  NodeDataBusClientImpl client(&adapter, "session-test");

  // Test contains
  EXPECT_FALSE(client.user()->contains("nonexistent"));

  client.user()->put("test_key", SoDataVariant(std::string("value")));
  EXPECT_TRUE(client.user()->contains("test_key"));

  // Test remove
  EXPECT_TRUE(client.user()->remove("test_key"));
  EXPECT_FALSE(client.user()->contains("test_key"));
  EXPECT_FALSE(client.user()->remove("nonexistent"));
}

// Test lazy initialization of namespace clients
TEST(NodeDataBusClientTest, LazyInitialization) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  NodeDataBusClientImpl client(&adapter, "session-lazy");

  // Access sys namespace - should be created on first access
  auto* sys1 = client.sys();
  ASSERT_NE(sys1, nullptr);

  // Access again - should return same instance
  auto* sys2 = client.sys();
  EXPECT_EQ(sys1, sys2);

  // Access different namespace - should be different instance
  auto* user = client.user();
  EXPECT_NE(sys1, user);
}

// Test with UUID format session ID
TEST(NodeDataBusClientTest, UuidSessionId) {
  MMDataBus databus;
  NodeDataBusAdapter adapter(&databus);

  // Use UUID format session ID
  std::string uuid_session = "550e8400-e29b-41d4-a716-446655440000";
  NodeDataBusClientImpl client(&adapter, uuid_session);

  EXPECT_EQ(client.session_id(), uuid_session);

  client.user()->put("data", SoDataVariant(std::string("value")));

  // Verify the key is properly prefixed with UUID
  SoDataVariant value;
  EXPECT_TRUE(adapter.get("/session/550e8400-e29b-41d4-a716-446655440000/user/data", value));
}

}  // namespace runtime
}  // namespace agentflow