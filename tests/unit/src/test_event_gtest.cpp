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
  agentflow::framework::MMAgentEvent::Config config;
  config.async_dispatch = false;
  
  agentflow::framework::MMAgentEvent event_mgr(config);
  
  agentflow::framework::MMAgentEvent::EventData event;
  event.type = agentflow::framework::MMAgentEvent::EventType::TASK_STARTED;
  event.source_id = "agent_1";
  
  EXPECT_TRUE(event_mgr.emit_event(event));
  EXPECT_EQ(event_mgr.get_recent_events().size(), 1);
}

TEST_F(AgentEventTest, EventSubscription) {
  agentflow::framework::MMAgentEvent::Config config;
  config.async_dispatch = false;
  
  agentflow::framework::MMAgentEvent event_mgr(config);
  
  bool received = false;
  auto listener = [&](const agentflow::framework::MMAgentEvent::EventData& e) {
    if (e.type == agentflow::framework::MMAgentEvent::EventType::MESSAGE_SENT) {
      received = true;
    }
  };
  
  event_mgr.register_listener(agentflow::framework::MMAgentEvent::EventType::MESSAGE_SENT, listener);
  
  agentflow::framework::MMAgentEvent::EventData event;
  event.type = agentflow::framework::MMAgentEvent::EventType::MESSAGE_SENT;
  
  event_mgr.emit_event(event);
  
  EXPECT_TRUE(received);
}

TEST_F(AgentEventTest, EventStoresDescriptionAsDataRef) {
  agentflow::framework::MMAgentEvent::Config config;
  config.async_dispatch = false;
  agentflow::framework::MMAgentEvent event_mgr(config);

  agentflow::framework::MMAgentEvent::EventData event;
  event.event_id = "event-dataref";
  event.type = agentflow::framework::MMAgentEvent::EventType::DATA_READY;
  event.description = "event-payload";
  event.attributes["agent_id"] = "agent-e";
  ASSERT_TRUE(event_mgr.emit_event(event));

  auto recent = event_mgr.get_recent_events(1);
  ASSERT_EQ(recent.size(), 1);
  EXPECT_TRUE(recent[0].description.empty());
  EXPECT_FALSE(recent[0].data_ref.empty());
}
