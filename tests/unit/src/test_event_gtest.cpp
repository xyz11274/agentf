// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test tests for Event system

#include <gtest/gtest.h>

#include "agent/event.h"

class AgentEventTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(AgentEventTest, EventCreation) {
  agentflow::framework::AgentEvent::Config config;
  config.async_dispatch = false;
  
  agentflow::framework::AgentEvent event_mgr(config);
  
  agentflow::framework::AgentEvent::EventData event;
  event.type = agentflow::framework::AgentEvent::EventType::TASK_STARTED;
  event.source_id = "agent_1";
  
  EXPECT_TRUE(event_mgr.emit_event(event));
  EXPECT_EQ(event_mgr.get_recent_events().size(), 1);
}

TEST_F(AgentEventTest, EventSubscription) {
  agentflow::framework::AgentEvent::Config config;
  config.async_dispatch = false;
  
  agentflow::framework::AgentEvent event_mgr(config);
  
  bool received = false;
  auto listener = [&](const agentflow::framework::AgentEvent::EventData& e) {
    if (e.type == agentflow::framework::AgentEvent::EventType::MESSAGE_SENT) {
      received = true;
    }
  };
  
  event_mgr.register_listener(agentflow::framework::AgentEvent::EventType::MESSAGE_SENT, listener);
  
  agentflow::framework::AgentEvent::EventData event;
  event.type = agentflow::framework::AgentEvent::EventType::MESSAGE_SENT;
  
  event_mgr.emit_event(event);
  
  EXPECT_TRUE(received);
}
