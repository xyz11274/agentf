#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/event_handler.h"
#include "runtime/agent_runtime.h"
#include "agent/agent.h"
#include "agent/session.h"

namespace agentflow {
namespace tests {

// =============================================================================
// EventType Tests
// =============================================================================

class EventTypeTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(EventTypeTest, EventTypeValues) {
  // Agent lifecycle events (0-9)
  EXPECT_EQ(static_cast<int>(runtime::EventType::AGENT_REGISTERED), 0);
  EXPECT_EQ(static_cast<int>(runtime::EventType::AGENT_UNREGISTERED), 1);

  // Session lifecycle events (10-19)
  EXPECT_EQ(static_cast<int>(runtime::EventType::SESSION_CREATED), 10);
  EXPECT_EQ(static_cast<int>(runtime::EventType::SESSION_DESTROYED), 11);

  // Dialogue lifecycle events (20-29)
  EXPECT_EQ(static_cast<int>(runtime::EventType::DIALOGUE_CREATED), 20);
  EXPECT_EQ(static_cast<int>(runtime::EventType::DIALOGUE_DESTROYED), 21);

  // Task lifecycle events (30-39)
  EXPECT_EQ(static_cast<int>(runtime::EventType::TASK_CREATED), 30);
  EXPECT_EQ(static_cast<int>(runtime::EventType::TASK_DESTROYED), 31);
}

// =============================================================================
// Event Structure Tests
// =============================================================================

class EventStructureTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(EventStructureTest, NodeInfoDefaultValues) {
  runtime::NodeInfo info;
  EXPECT_EQ(info.priority, 0);
  EXPECT_TRUE(info.id.empty());
  EXPECT_TRUE(info.name.empty());
  EXPECT_TRUE(info.type.empty());
  EXPECT_TRUE(info.input_params.empty());
  EXPECT_TRUE(info.output_params.empty());
}

TEST_F(EventStructureTest, EdgeInfoDefaultValues) {
  runtime::EdgeInfo info;
  EXPECT_TRUE(info.id.empty());
  EXPECT_TRUE(info.source_node_id.empty());
  EXPECT_TRUE(info.target_node_id.empty());
  EXPECT_TRUE(info.condition.empty());
}

TEST_F(EventStructureTest, WorkflowInfoDefaultValues) {
  runtime::WorkflowInfo info;
  EXPECT_TRUE(info.id.empty());
  EXPECT_TRUE(info.name.empty());
  EXPECT_TRUE(info.version.empty());
  EXPECT_TRUE(info.description.empty());
  EXPECT_TRUE(info.nodes.empty());
  EXPECT_TRUE(info.edges.empty());
}

TEST_F(EventStructureTest, AgentEventDefaultValues) {
  runtime::AgentEvent event;
  EXPECT_TRUE(event.agent_id.empty());
  EXPECT_TRUE(event.name.empty());
  EXPECT_TRUE(event.version.empty());
  EXPECT_TRUE(event.description.empty());
  EXPECT_TRUE(event.workflows.empty());
}

TEST_F(EventStructureTest, SessionEventDefaultValues) {
  runtime::SessionEvent event;
  EXPECT_TRUE(event.agent_id.empty());
  EXPECT_TRUE(event.user_id.empty());
  EXPECT_TRUE(event.client_id.empty());
  EXPECT_TRUE(event.session_id.empty());
}

TEST_F(EventStructureTest, DialogueEventDefaultValues) {
  runtime::DialogueEvent event;
  EXPECT_TRUE(event.agent_id.empty());
  EXPECT_TRUE(event.user_id.empty());
  EXPECT_TRUE(event.client_id.empty());
  EXPECT_TRUE(event.session_id.empty());
  EXPECT_TRUE(event.dialogue_id.empty());
  EXPECT_TRUE(event.user_input.empty());
}

TEST_F(EventStructureTest, TaskEventDefaultValues) {
  runtime::TaskEvent event;
  EXPECT_TRUE(event.agent_id.empty());
  EXPECT_TRUE(event.user_id.empty());
  EXPECT_TRUE(event.client_id.empty());
  EXPECT_TRUE(event.session_id.empty());
  EXPECT_TRUE(event.dialogue_id.empty());
  EXPECT_TRUE(event.task_id.empty());
  EXPECT_TRUE(event.workflow_id.empty());
  EXPECT_TRUE(event.task_type.empty());
  EXPECT_TRUE(event.skill_name.empty());
}

// =============================================================================
// EventBuilder Tests
// =============================================================================

class EventBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(EventBuilderTest, BuildAgentRegisteredEvent) {
  std::vector<runtime::WorkflowInfo> workflows;
  runtime::WorkflowInfo wf_info;
  wf_info.id = "workflow-1";
  wf_info.name = "Test Workflow";
  workflows.push_back(wf_info);

  runtime::AgentEvent event = runtime::EventBuilder::build_agent_registered_event(
      "agent-1", "TestAgent", "1.0", "Test Description", workflows);

  EXPECT_EQ(event.event_type, runtime::EventType::AGENT_REGISTERED);
  EXPECT_EQ(event.agent_id, "agent-1");
  EXPECT_EQ(event.name, "TestAgent");
  EXPECT_EQ(event.version, "1.0");
  EXPECT_EQ(event.description, "Test Description");
  EXPECT_EQ(event.workflows.size(), 1u);
  EXPECT_EQ(event.workflows[0].id, "workflow-1");
}

TEST_F(EventBuilderTest, BuildAgentUnregisteredEvent) {
  runtime::AgentEvent event = runtime::EventBuilder::build_agent_unregistered_event("agent-1");

  EXPECT_EQ(event.event_type, runtime::EventType::AGENT_UNREGISTERED);
  EXPECT_EQ(event.agent_id, "agent-1");
}

TEST_F(EventBuilderTest, BuildSessionCreatedEvent) {
  runtime::SessionEvent event = runtime::EventBuilder::build_session_created_event(
      "agent-1", "user-1", "client-1", "session-1");

  EXPECT_EQ(event.event_type, runtime::EventType::SESSION_CREATED);
  EXPECT_EQ(event.agent_id, "agent-1");
  EXPECT_EQ(event.user_id, "user-1");
  EXPECT_EQ(event.client_id, "client-1");
  EXPECT_EQ(event.session_id, "session-1");
}

TEST_F(EventBuilderTest, BuildSessionDestroyedEvent) {
  runtime::SessionEvent event = runtime::EventBuilder::build_session_destroyed_event(
      "agent-1", "session-1");

  EXPECT_EQ(event.event_type, runtime::EventType::SESSION_DESTROYED);
  EXPECT_EQ(event.agent_id, "agent-1");
  EXPECT_EQ(event.session_id, "session-1");
}

TEST_F(EventBuilderTest, BuildDialogueCreatedEvent) {
  runtime::DialogueEvent event = runtime::EventBuilder::build_dialogue_created_event(
      "agent-1", "user-1", "client-1", "session-1", "dialogue-1", "Hello");

  EXPECT_EQ(event.event_type, runtime::EventType::DIALOGUE_CREATED);
  EXPECT_EQ(event.agent_id, "agent-1");
  EXPECT_EQ(event.user_id, "user-1");
  EXPECT_EQ(event.client_id, "client-1");
  EXPECT_EQ(event.session_id, "session-1");
  EXPECT_EQ(event.dialogue_id, "dialogue-1");
  EXPECT_EQ(event.user_input, "Hello");
}

TEST_F(EventBuilderTest, BuildDialogueDestroyedEvent) {
  runtime::DialogueEvent event = runtime::EventBuilder::build_dialogue_destroyed_event(
      "agent-1", "session-1", "dialogue-1");

  EXPECT_EQ(event.event_type, runtime::EventType::DIALOGUE_DESTROYED);
  EXPECT_EQ(event.agent_id, "agent-1");
  EXPECT_EQ(event.session_id, "session-1");
  EXPECT_EQ(event.dialogue_id, "dialogue-1");
}

TEST_F(EventBuilderTest, BuildTaskCreatedEvent) {
  runtime::TaskEvent event = runtime::EventBuilder::build_task_created_event(
      "agent-1", "user-1", "client-1", "session-1", "dialogue-1",
      "task-1", "workflow-1", "skill", "skill-name");

  EXPECT_EQ(event.event_type, runtime::EventType::TASK_CREATED);
  EXPECT_EQ(event.agent_id, "agent-1");
  EXPECT_EQ(event.user_id, "user-1");
  EXPECT_EQ(event.client_id, "client-1");
  EXPECT_EQ(event.session_id, "session-1");
  EXPECT_EQ(event.dialogue_id, "dialogue-1");
  EXPECT_EQ(event.task_id, "task-1");
  EXPECT_EQ(event.workflow_id, "workflow-1");
  EXPECT_EQ(event.task_type, "skill");
  EXPECT_EQ(event.skill_name, "skill-name");
}

TEST_F(EventBuilderTest, BuildTaskDestroyedEvent) {
  runtime::TaskEvent event = runtime::EventBuilder::build_task_destroyed_event(
      "agent-1", "session-1", "dialogue-1", "task-1");

  EXPECT_EQ(event.event_type, runtime::EventType::TASK_DESTROYED);
  EXPECT_EQ(event.agent_id, "agent-1");
  EXPECT_EQ(event.session_id, "session-1");
  EXPECT_EQ(event.dialogue_id, "dialogue-1");
  EXPECT_EQ(event.task_id, "task-1");
}

// =============================================================================
// Mock Event Handler
// =============================================================================

class MockEventHandler : public runtime::IEventHandler {
 public:
  std::vector<runtime::AgentEvent> agent_events;
  std::vector<runtime::SessionEvent> session_events;
  std::vector<runtime::DialogueEvent> dialogue_events;
  std::vector<runtime::TaskEvent> task_events;
  std::mutex mutex;

  void handle_agent_event(const runtime::AgentEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex);
    agent_events.push_back(event);
  }

  void handle_session_event(const runtime::SessionEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex);
    session_events.push_back(event);
  }

  void handle_dialogue_event(const runtime::DialogueEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex);
    dialogue_events.push_back(event);
  }

  void handle_task_event(const runtime::TaskEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex);
    task_events.push_back(event);
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex);
    agent_events.clear();
    session_events.clear();
    dialogue_events.clear();
    task_events.clear();
  }
};

// =============================================================================
// Runtime Event Handler Tests
// =============================================================================

class RuntimeEventHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    runtime_ = std::make_unique<runtime::MMAgentRuntime>();
    handler_ = std::make_shared<MockEventHandler>();
  }

  void TearDown() override {
    runtime_.reset();
    handler_.reset();
  }

  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::shared_ptr<MockEventHandler> handler_;
};

TEST_F(RuntimeEventHandlerTest, RegisterEventHandler) {
  EXPECT_EQ(runtime_->get_event_handler_count(), 0u);
  runtime_->register_event_handler(handler_);
  EXPECT_EQ(runtime_->get_event_handler_count(), 1u);
}

TEST_F(RuntimeEventHandlerTest, UnregisterEventHandler) {
  runtime_->register_event_handler(handler_);
  EXPECT_EQ(runtime_->get_event_handler_count(), 1u);

  runtime_->unregister_event_handler(handler_);
  EXPECT_EQ(runtime_->get_event_handler_count(), 0u);
}

TEST_F(RuntimeEventHandlerTest, ClearEventHandlers) {
  auto handler2 = std::make_shared<MockEventHandler>();
  runtime_->register_event_handler(handler_);
  runtime_->register_event_handler(handler2);
  EXPECT_EQ(runtime_->get_event_handler_count(), 2u);

  runtime_->clear_event_handlers();
  EXPECT_EQ(runtime_->get_event_handler_count(), 0u);
}

TEST_F(RuntimeEventHandlerTest, RegisterNullHandler) {
  runtime_->register_event_handler(nullptr);
  EXPECT_EQ(runtime_->get_event_handler_count(), 0u);
}

TEST_F(RuntimeEventHandlerTest, CallbackThreadPoolSize) {
  EXPECT_EQ(runtime_->get_callback_thread_pool_size(), 5u);

  runtime_->set_callback_thread_pool_size(10);
  EXPECT_EQ(runtime_->get_callback_thread_pool_size(), 10u);

  // Setting to 0 should default to 5
  runtime_->set_callback_thread_pool_size(0);
  EXPECT_EQ(runtime_->get_callback_thread_pool_size(), 5u);
}

// =============================================================================
// Session Event Callback Tests
// =============================================================================

class SessionEventCallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create session with default config
    framework::MMAgentSession::Config config;
    config.session_id = "test-session";
    session_ = std::make_shared<framework::MMAgentSession>(config);
  }

  void TearDown() override {
    session_.reset();
  }

  std::shared_ptr<framework::MMAgentSession> session_;
};

TEST_F(SessionEventCallbackTest, BindRouteTriggersCallback) {
  std::string last_session_id;
  std::string last_user_id;
  std::string last_client_id;
  framework::SessionEventType last_event_type;

  session_->set_session_event_callback(
      [&](const std::string& session_id, const std::string& user_id,
          const std::string& client_id, framework::SessionEventType event_type) {
        last_session_id = session_id;
        last_user_id = user_id;
        last_client_id = client_id;
        last_event_type = event_type;
      });

  session_->bind_route("user-1", "client-1");

  EXPECT_EQ(last_session_id, "test-session");
  EXPECT_EQ(last_user_id, "user-1");
  EXPECT_EQ(last_client_id, "client-1");
  EXPECT_EQ(last_event_type, framework::SessionEventType::CREATED);
}

TEST_F(SessionEventCallbackTest, ResetForPoolTriggersCallback) {
  std::vector<framework::SessionEventType> events;

  session_->set_session_event_callback(
      [&](const std::string& session_id, const std::string& user_id,
          const std::string& client_id, framework::SessionEventType event_type) {
        (void)session_id;
        (void)user_id;
        (void)client_id;
        events.push_back(event_type);
      });

  // First bind, then reset
  session_->bind_route("user-1", "client-1");
  session_->reset_for_pool();

  EXPECT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], framework::SessionEventType::CREATED);
  EXPECT_EQ(events[1], framework::SessionEventType::DESTROYED);
}

TEST_F(SessionEventCallbackTest, ResetWithoutBindDoesNotTriggerCallback) {
  int callback_count = 0;

  session_->set_session_event_callback(
      [&](const std::string& session_id, const std::string& user_id,
          const std::string& client_id, framework::SessionEventType event_type) {
        (void)session_id;
        (void)user_id;
        (void)client_id;
        (void)event_type;
        callback_count++;
      });

  // Reset without bind should not trigger DESTROYED callback
  session_->reset_for_pool();

  EXPECT_EQ(callback_count, 0);
}

TEST_F(SessionEventCallbackTest, ClearCallback) {
  int callback_count = 0;

  session_->set_session_event_callback(
      [&](const std::string& session_id, const std::string& user_id,
          const std::string& client_id, framework::SessionEventType event_type) {
        (void)session_id;
        (void)user_id;
        (void)client_id;
        (void)event_type;
        callback_count++;
      });

  session_->bind_route("user-1", "client-1");
  EXPECT_EQ(callback_count, 1);

  session_->clear_session_event_callback();
  session_->reset_for_pool();

  // Callback was cleared, so count should still be 1
  EXPECT_EQ(callback_count, 1);
}

}  // namespace tests
}  // namespace agentflow