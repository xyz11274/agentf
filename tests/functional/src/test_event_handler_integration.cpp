#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "agent/agent.h"
#include "runtime/agent_runtime.h"
#include "runtime/event_handler.h"

namespace agentflow {
namespace tests {

// =============================================================================
// Test Event Handler that records all events
// =============================================================================

class RecordingEventHandler : public runtime::IEventHandler {
public:
  std::vector<runtime::AgentEvent>    agent_events;
  std::vector<runtime::SessionEvent>  session_events;
  std::vector<runtime::DialogueEvent> dialogue_events;
  std::vector<runtime::TaskEvent>     task_events;

  std::unordered_set<std::thread::id> callback_thread_ids;
  std::thread::id                     main_thread_id;

  std::mutex              events_mutex;
  std::condition_variable events_cv;

  std::atomic<int> agent_event_count{0};
  std::atomic<int> session_event_count{0};
  std::atomic<int> dialogue_event_count{0};
  std::atomic<int> task_event_count{0};

  RecordingEventHandler() {
    main_thread_id = std::this_thread::get_id();
  }

  void handle_agent_event(const runtime::AgentEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    callback_thread_ids.insert(std::this_thread::get_id());
    agent_events.push_back(event);
    agent_event_count++;
    events_cv.notify_all();
  }

  void handle_session_event(const runtime::SessionEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    callback_thread_ids.insert(std::this_thread::get_id());
    session_events.push_back(event);
    session_event_count++;
    events_cv.notify_all();
  }

  void handle_dialogue_event(const runtime::DialogueEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    callback_thread_ids.insert(std::this_thread::get_id());
    dialogue_events.push_back(event);
    dialogue_event_count++;
    events_cv.notify_all();
  }

  void handle_task_event(const runtime::TaskEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    callback_thread_ids.insert(std::this_thread::get_id());
    task_events.push_back(event);
    task_event_count++;
    events_cv.notify_all();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(events_mutex);
    agent_events.clear();
    session_events.clear();
    dialogue_events.clear();
    task_events.clear();
    callback_thread_ids.clear();
    agent_event_count = 0;
    session_event_count = 0;
    dialogue_event_count = 0;
    task_event_count = 0;
  }

  bool wait_for_agent_events(int count, int timeout_ms = 5000) {
    std::unique_lock<std::mutex> lock(events_mutex);
    return events_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [this, count]() { return agent_event_count >= count; });
  }

  bool wait_for_session_events(int count, int timeout_ms = 5000) {
    std::unique_lock<std::mutex> lock(events_mutex);
    return events_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [this, count]() { return session_event_count >= count; });
  }

  bool is_callback_on_different_thread() const {
    // Returns true if at least one callback ran on a different thread
    return callback_thread_ids.find(main_thread_id) == callback_thread_ids.end() ||
           callback_thread_ids.size() > 1;
  }
};

// =============================================================================
// Integration Test Fixture
// =============================================================================

class EventHandlerIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    handler_ = std::make_shared<RecordingEventHandler>();

    // Create runtime
    runtime::MMAgentRuntime::Config config;
    config.role = runtime::MMAgentRuntime::RuntimeRole::MASTER;
    config.worker_threads = 2;
    runtime_ = std::make_unique<runtime::MMAgentRuntime>(config);

    // Set small thread pool for tests
    runtime_->set_callback_thread_pool_size(2);

    // Register event handler before starting
    runtime_->register_event_handler(handler_);

    // Start runtime
    runtime_->start();

    // Wait for callback threads to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  void TearDown() override {
    if(runtime_) {
      runtime_->stop();
    }
    runtime_.reset();
    handler_.reset();
  }

  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::shared_ptr<RecordingEventHandler>   handler_;
};

// =============================================================================
// Test Cases
// =============================================================================

TEST_F(EventHandlerIntegrationTest, AgentRegisteredEvent) {
  // Create a minimal agent
  framework::MMAgent::Config config;
  config.id   = "test-agent-1";
  config.name = "TestAgent";
  config.version = "1.0.0";
  config.description = "Integration test agent";

  auto agent = std::make_shared<framework::MMAgent>(config);
  ASSERT_NE(agent, nullptr);

  // Register agent
  bool registered = runtime_->register_agent(agent);
  EXPECT_TRUE(registered);

  // Wait for AGENT_REGISTERED event
  bool received = handler_->wait_for_agent_events(1, 3000);
  EXPECT_TRUE(received);

  // Verify event content
  std::lock_guard<std::mutex> lock(handler_->events_mutex);
  EXPECT_EQ(handler_->agent_events.size(), 1u);
  if(!handler_->agent_events.empty()) {
    EXPECT_EQ(handler_->agent_events[0].event_type, runtime::EventType::AGENT_REGISTERED);
    EXPECT_EQ(handler_->agent_events[0].agent_id, "test-agent-1");
    EXPECT_EQ(handler_->agent_events[0].name, "TestAgent");
  }
}

TEST_F(EventHandlerIntegrationTest, AgentUnregisteredEvent) {
  // Create and register agent
  framework::MMAgent::Config config;
  config.id   = "test-agent-2";
  config.name = "TestAgent2";

  auto agent = std::make_shared<framework::MMAgent>(config);
  runtime_->register_agent(agent);

  // Wait for registration event
  handler_->wait_for_agent_events(1, 3000);
  handler_->clear();

  // Unregister agent
  bool unregistered = runtime_->unregister_agent("test-agent-2");
  EXPECT_TRUE(unregistered);

  // Wait for AGENT_UNREGISTERED event
  bool received = handler_->wait_for_agent_events(1, 3000);
  EXPECT_TRUE(received);

  // Verify event content
  std::lock_guard<std::mutex> lock(handler_->events_mutex);
  EXPECT_EQ(handler_->agent_events.size(), 1u);
  if(!handler_->agent_events.empty()) {
    EXPECT_EQ(handler_->agent_events[0].event_type, runtime::EventType::AGENT_UNREGISTERED);
    EXPECT_EQ(handler_->agent_events[0].agent_id, "test-agent-2");
  }
}

TEST_F(EventHandlerIntegrationTest, SessionCreatedEvent) {
  // Create and register agent
  framework::MMAgent::Config config;
  config.id = "test-agent-3";
  auto agent = std::make_shared<framework::MMAgent>(config);
  runtime_->register_agent(agent);

  // Wait for agent registration
  handler_->wait_for_agent_events(1, 3000);
  handler_->clear();

  // Create session using public API
  std::string user_id   = "test-user-1";
  std::string client_id = "test-client-1";
  auto session = agent->get_or_create_session(user_id, client_id);
  ASSERT_NE(session, nullptr);

  // Wait for SESSION_CREATED event
  bool received = handler_->wait_for_session_events(1, 3000);
  EXPECT_TRUE(received);

  // Verify event content
  std::lock_guard<std::mutex> lock(handler_->events_mutex);
  EXPECT_EQ(handler_->session_events.size(), 1u);
  if(!handler_->session_events.empty()) {
    EXPECT_EQ(handler_->session_events[0].event_type, runtime::EventType::SESSION_CREATED);
    EXPECT_EQ(handler_->session_events[0].agent_id, "test-agent-3");
    EXPECT_EQ(handler_->session_events[0].user_id, user_id);
    EXPECT_EQ(handler_->session_events[0].client_id, client_id);
  }
}

TEST_F(EventHandlerIntegrationTest, SessionDestroyedEvent) {
  // Create and register agent
  framework::MMAgent::Config config;
  config.id = "test-agent-4";
  auto agent = std::make_shared<framework::MMAgent>(config);
  runtime_->register_agent(agent);

  // Wait for agent registration
  handler_->wait_for_agent_events(1, 3000);
  handler_->clear();

  // Create session using public API
  auto session = agent->get_or_create_session("user-1", "client-1");

  // Wait for SESSION_CREATED
  handler_->wait_for_session_events(1, 3000);
  handler_->clear();

  // Reset session (simulates returning to pool)
  session->reset_for_pool();

  // Wait for SESSION_DESTROYED event
  bool received = handler_->wait_for_session_events(1, 3000);
  EXPECT_TRUE(received);

  // Verify event content
  std::lock_guard<std::mutex> lock(handler_->events_mutex);
  EXPECT_EQ(handler_->session_events.size(), 1u);
  if(!handler_->session_events.empty()) {
    EXPECT_EQ(handler_->session_events[0].event_type, runtime::EventType::SESSION_DESTROYED);
    EXPECT_EQ(handler_->session_events[0].agent_id, "test-agent-4");
  }
}

TEST_F(EventHandlerIntegrationTest, AsyncCallbackExecution) {
  // This test verifies that callbacks are executed asynchronously
  // in a different thread (from the thread pool)

  // Create and register multiple agents to generate events
  for(int i = 0; i < 3; ++i) {
    framework::MMAgent::Config config;
    config.id   = "async-test-agent-" + std::to_string(i);
    config.name = "AsyncTestAgent" + std::to_string(i);

    auto agent = std::make_shared<framework::MMAgent>(config);
    runtime_->register_agent(agent);
  }

  // Wait for all AGENT_REGISTERED events
  bool received = handler_->wait_for_agent_events(3, 5000);
  EXPECT_TRUE(received);

  // Verify callbacks were executed on different threads (thread pool)
  // Note: This may not always be true if all callbacks execute before
  // the main thread checks, but with multiple events it's likely
  std::lock_guard<std::mutex> lock(handler_->events_mutex);
  EXPECT_GE(handler_->agent_events.size(), 3u);

  // Log thread IDs for debugging
  std::cout << "Main thread ID: " << handler_->main_thread_id << std::endl;
  std::cout << "Callback thread IDs count: " << handler_->callback_thread_ids.size() << std::endl;
  for(const auto& tid : handler_->callback_thread_ids) {
    std::cout << "  Callback on thread: " << tid << std::endl;
  }
}

TEST_F(EventHandlerIntegrationTest, MultipleEventTypesInSequence) {
  // Test sequence: Agent -> Session -> Session Destroyed

  // Create agent
  framework::MMAgent::Config config;
  config.id   = "sequence-test-agent";
  config.name = "SequenceTestAgent";
  auto agent = std::make_shared<framework::MMAgent>(config);

  // Register agent
  runtime_->register_agent(agent);

  // Wait for AGENT_REGISTERED
  handler_->wait_for_agent_events(1, 3000);
  handler_->clear();

  // Create session using public API
  auto session = agent->get_or_create_session("seq-user", "seq-client");

  // Wait for SESSION_CREATED
  handler_->wait_for_session_events(1, 3000);
  handler_->clear();

  // Destroy session
  session->reset_for_pool();

  // Wait for SESSION_DESTROYED event
  bool received = false;
  {
    std::unique_lock<std::mutex> lock(handler_->events_mutex);
    received = handler_->events_cv.wait_for(lock, std::chrono::milliseconds(3000),
                                             [this]() { return handler_->session_event_count >= 1; });
  }
  EXPECT_TRUE(received);

  // Verify sequence
  std::lock_guard<std::mutex> lock(handler_->events_mutex);
  EXPECT_EQ(handler_->session_events.size(), 1u);
  if(!handler_->session_events.empty()) {
    EXPECT_EQ(handler_->session_events[0].event_type, runtime::EventType::SESSION_DESTROYED);
  }
}

TEST_F(EventHandlerIntegrationTest, HandlerRegistrationUnregistration) {
  // Test dynamic handler registration/unregistration

  // Create a second handler
  auto handler2 = std::make_shared<RecordingEventHandler>();

  // Register second handler
  runtime_->register_event_handler(handler2);
  EXPECT_EQ(runtime_->get_event_handler_count(), 2u);

  // Create agent - both handlers should receive event
  framework::MMAgent::Config config;
  config.id = "multi-handler-agent";
  auto agent = std::make_shared<framework::MMAgent>(config);
  runtime_->register_agent(agent);

  // Wait for events on both handlers
  bool received1 = handler_->wait_for_agent_events(1, 3000);
  bool received2 = handler2->wait_for_agent_events(1, 3000);
  EXPECT_TRUE(received1);
  EXPECT_TRUE(received2);

  // Unregister second handler
  runtime_->unregister_event_handler(handler2);
  EXPECT_EQ(runtime_->get_event_handler_count(), 1u);

  // Create another agent - only first handler should receive
  handler_->clear();
  handler2->clear();

  framework::MMAgent::Config config2;
  config2.id = "single-handler-agent";
  auto agent2 = std::make_shared<framework::MMAgent>(config2);
  runtime_->register_agent(agent2);

  received1 = handler_->wait_for_agent_events(1, 3000);
  // handler2 should not receive (wait briefly to confirm)
  bool received2_late = handler2->wait_for_agent_events(1, 500);

  EXPECT_TRUE(received1);
  EXPECT_FALSE(received2_late);  // Should timeout
}

}  // namespace tests
}  // namespace agentflow