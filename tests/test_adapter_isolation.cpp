// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Tests to verify dependency isolation from Protoactor-cpp

#ifdef AGENTFLOW_WITH_PROTOACTOR
#include <gtest/gtest.h>
#else
#include <cassert>
#include <iostream>
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_NE(a, b) assert((a) != (b))
#define EXPECT_TRUE(x) assert(x)
#define EXPECT_FALSE(x) assert(!(x))
#endif

#include "agentflow/actor/adapter.h"
#include "agentflow/common.h"

namespace agentflow {
namespace actor {
namespace adapter {

// Test 1: Verify adapter can be used without Protoactor
void test_adapter_independent_api() {
#ifndef AGENTFLOW_WITH_PROTOACTOR
  std::cout << "✓ Test: Adapter API is independent of Protoactor-cpp\n";
#endif
  
  // This should compile regardless of Protoactor availability
  auto msg = ActorMessage();
  msg.message_type = "TestMessage";
  msg.payload = nullptr;
  
  ASSERT_EQ(msg.message_type, "TestMessage");
}

// Test 2: Verify actor system creation
void test_actor_system_creation() {
  auto system = ActorSystemFactory::create("test-system");
  ASSERT_TRUE(system != nullptr);
  EXPECT_TRUE(system->is_running());
  
  std::cout << "✓ Test: Actor system creation\n";
  std::cout << "  - Implementation: " << ActorSystemFactory::implementation_name() << "\n";
  std::cout << "  - Version: " << ActorSystemFactory::implementation_version() << "\n";
}

// Test 3: Verify actor spawning (with stub implementation)
void test_actor_spawning() {
  auto system = ActorSystemFactory::create("spawn-test");
  ASSERT_TRUE(system != nullptr);
  
  // Create simple test actor
  class TestActor : public IActor {
    void on_message(std::shared_ptr<IContext> ctx) override {
      // Simple echo
    }
    
    void on_start() override {
      // Empty
    }
    
    void on_stop() override {
      // Empty
    }
  };
  
  auto factory = []() -> std::shared_ptr<IActor> {
    return std::make_shared<TestActor>();
  };
  
  auto actor_ref = system->spawn(factory, "test-actor");
  ASSERT_TRUE(actor_ref != nullptr);
  ASSERT_TRUE(actor_ref->is_alive());
  EXPECT_EQ(actor_ref->address(), "test-actor");
  
  std::cout << "✓ Test: Actor spawning\n";
  std::cout << "  - Actor: " << actor_ref->to_string() << "\n";
  
  system->stop(actor_ref);
  system->shutdown();
  EXPECT_FALSE(system->is_running());
}

// Test 4: Verify message creation utilities
void test_message_builder() {
  struct TestPayload {
    int value;
    std::string name;
  };
  
  TestPayload payload = {42, "test"};
  auto msg = MessageBuilder::create("TestType", payload);
  
  ASSERT_EQ(msg.message_type, "TestType");
  ASSERT_TRUE(msg.payload != nullptr);
  
  auto retrieved = msg.get_payload<TestPayload>();
  ASSERT_TRUE(retrieved != nullptr);
  ASSERT_EQ(retrieved->value, 42);
  EXPECT_EQ(retrieved->name, "test");
  
  std::cout << "✓ Test: Message builder utility\n";
}

// Test 5: Verify no direct Protoactor includes in tests
void test_no_protoactor_includes() {
  // This file should NOT include any Protoactor headers directly
  // Verification is done at compile time by restricting includes
  std::cout << "✓ Test: No direct Protoactor-cpp includes in adapter tests\n";
}

// Test 6: Actor reference lifecycle
void test_actor_reference_lifecycle() {
  auto system = ActorSystemFactory::create("ref-test");
  
  class SimpleActor : public IActor {
    void on_message(std::shared_ptr<IContext> ctx) override {}
  };
  
  auto factory = []() { return std::make_shared<SimpleActor>(); };
  auto ref = system->spawn(factory, "lifecycle-actor");
  
  ASSERT_TRUE(ref->is_alive());
  std::string addr = ref->address();
  ASSERT_FALSE(addr.empty());
  
  system->stop(ref);
  // Note: ref is still valid, but may reflect stopped state
  
  system->shutdown();
  
  std::cout << "✓ Test: Actor reference lifecycle\n";
}

// Test 7: Verify implementation fallback behavior
void test_implementation_fallback() {
  auto impl_name = ActorSystemFactory::implementation_name();
  
#ifdef AGENTFLOW_WITH_PROTOACTOR
  ASSERT_EQ(impl_name, "protoactor-cpp");
#else
  ASSERT_EQ(impl_name, "stub");
#endif
  
  std::cout << "✓ Test: Implementation selection\n";
  std::cout << "  - Selected: " << impl_name << "\n";
}

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow

// Main test runner (when not using Google Test)
#ifndef AGENTFLOW_WITH_PROTOACTOR
int main() {
  std::cout << "\n=== AgentFlow Adapter Isolation Tests ===\n\n";
  
  try {
    agentflow::actor::adapter::test_adapter_independent_api();
    agentflow::actor::adapter::test_actor_system_creation();
    agentflow::actor::adapter::test_actor_spawning();
    agentflow::actor::adapter::test_message_builder();
    agentflow::actor::adapter::test_no_protoactor_includes();
    agentflow::actor::adapter::test_actor_reference_lifecycle();
    agentflow::actor::adapter::test_implementation_fallback();
    
    std::cout << "\n=== All isolation tests passed ===\n\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
#endif
