// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Tests to verify dependency isolation from Protoactor-cpp

#include <gtest/gtest.h>
#include <iostream>

#include "agentflow/actor/adapter.h"
#include "agentflow/common.h"

using namespace agentflow::actor::adapter;

// Test 1: Verify adapter can be used without Protoactor
TEST(AdapterIsolation, IndependentAPI) {
  auto msg         = ActorMessage();
  msg.message_type = "TestMessage";
  msg.payload      = nullptr;

  EXPECT_EQ(msg.message_type, "TestMessage");
}

// Test 2: Verify actor system creation
TEST(AdapterIsolation, ActorSystemCreation) {
  auto system = ActorSystemFactory::create("test-system");
  ASSERT_TRUE(system != nullptr);
  EXPECT_TRUE(system->is_running());

  std::cout << "Implementation: " << ActorSystemFactory::implementation_name() << std::endl;
  std::cout << "Version: " << ActorSystemFactory::implementation_version() << std::endl;

  system->shutdown();
}

// Test 3: Verify actor spawning (with stub implementation)
TEST(AdapterIsolation, ActorSpawning) {
  auto system = ActorSystemFactory::create("spawn-test");
  ASSERT_TRUE(system != nullptr);

  class TestActor : public IActor {
    void on_message(std::shared_ptr<IContext> ctx) override { (void)ctx; }
    void on_start() override {}
    void on_stop() override {}
  };

  auto factory = []() -> std::shared_ptr<IActor> { return std::make_shared<TestActor>(); };

  auto actor_ref = system->spawn(factory, "test-actor");
  ASSERT_TRUE(actor_ref != nullptr);
  EXPECT_TRUE(actor_ref->is_alive());
  EXPECT_EQ(actor_ref->address(), "test-actor");

  system->stop(actor_ref);
  system->shutdown();
  EXPECT_FALSE(system->is_running());
}

// Test 4: Verify message creation utilities
TEST(AdapterIsolation, MessageBuilder) {
  struct TestPayload {
    int         value;
    std::string name;
  };

  TestPayload payload = { 42, "test" };
  auto        msg     = MessageBuilder::create("TestType", payload);

  EXPECT_EQ(msg.message_type, "TestType");
  ASSERT_TRUE(msg.payload != nullptr);

  auto retrieved = msg.get_payload<TestPayload>();
  ASSERT_TRUE(retrieved != nullptr);
  EXPECT_EQ(retrieved->value, 42);
  EXPECT_EQ(retrieved->name, "test");
}

// Test 5: Actor reference lifecycle
TEST(AdapterIsolation, ActorReferenceLifecycle) {
  auto system = ActorSystemFactory::create("ref-test");

  class SimpleActor : public IActor {
    void on_message(std::shared_ptr<IContext> ctx) override { (void)ctx; }
  };

  auto factory = []() { return std::make_shared<SimpleActor>(); };
  auto ref     = system->spawn(factory, "lifecycle-actor");

  ASSERT_TRUE(ref->is_alive());
  std::string addr = ref->address();
  EXPECT_FALSE(addr.empty());

  system->stop(ref);
  system->shutdown();
}

// Test 6: Verify implementation fallback behavior
TEST(AdapterIsolation, ImplementationFallback) {
  auto impl_name = ActorSystemFactory::implementation_name();

#ifdef AGENTFLOW_WITH_PROTOACTOR
  EXPECT_EQ(impl_name, "protoactor-cpp");
#else
  EXPECT_EQ(impl_name, "stub");
#endif
}
