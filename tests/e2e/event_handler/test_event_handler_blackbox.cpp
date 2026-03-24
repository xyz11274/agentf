/**
 * @file test_event_handler_blackbox.cpp
 * @brief Black-box E2E test for event handler mechanism using SystemAgentMock
 *
 * This test verifies that lifecycle events are correctly emitted during
 * a real agent workflow execution, including:
 * - AGENT_REGISTERED event when agent is registered
 * - SESSION_CREATED event when session is created
 * - DIALOGUE_CREATED event when dialogue starts
 * - TASK_CREATED event when tasks are launched
 * - TASK_DESTROYED event when tasks complete
 * - DIALOGUE_DESTROYED event when dialogue ends
 * - SESSION_DESTROYED event when session is cleaned up
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "agent/agent.h"
#include "agent/session.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"
#include "runtime/event_handler.h"

namespace agentflow {
namespace tests {

// =============================================================================
// Test Event Handler that records all events with timing
// =============================================================================

class BlackboxEventHandler : public runtime::IEventHandler {
public:
  struct TimestampedAgentEvent {
    runtime::AgentEvent event;
    std::chrono::steady_clock::time_point timestamp;
  };
  struct TimestampedSessionEvent {
    runtime::SessionEvent event;
    std::chrono::steady_clock::time_point timestamp;
  };
  struct TimestampedDialogueEvent {
    runtime::DialogueEvent event;
    std::chrono::steady_clock::time_point timestamp;
  };
  struct TimestampedTaskEvent {
    runtime::TaskEvent event;
    std::chrono::steady_clock::time_point timestamp;
  };

  std::vector<TimestampedAgentEvent>    agent_events;
  std::vector<TimestampedSessionEvent>  session_events;
  std::vector<TimestampedDialogueEvent> dialogue_events;
  std::vector<TimestampedTaskEvent>     task_events;

  std::mutex              events_mutex;
  std::condition_variable events_cv;

  std::atomic<int> total_events{0};

  void handle_agent_event(const runtime::AgentEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    agent_events.push_back({event, std::chrono::steady_clock::now()});
    total_events++;
    events_cv.notify_all();
  }

  void handle_session_event(const runtime::SessionEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    session_events.push_back({event, std::chrono::steady_clock::now()});
    total_events++;
    events_cv.notify_all();
  }

  void handle_dialogue_event(const runtime::DialogueEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    dialogue_events.push_back({event, std::chrono::steady_clock::now()});
    total_events++;
    events_cv.notify_all();
  }

  void handle_task_event(const runtime::TaskEvent& event) override {
    std::lock_guard<std::mutex> lock(events_mutex);
    task_events.push_back({event, std::chrono::steady_clock::now()});
    total_events++;
    events_cv.notify_all();
  }

  bool wait_for_events(int count, int timeout_ms = 10000) {
    std::unique_lock<std::mutex> lock(events_mutex);
    return events_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [this, count]() { return total_events >= count; });
  }

  void print_summary() {
    std::lock_guard<std::mutex> lock(events_mutex);
    std::cout << "\n=== Event Summary ===" << std::endl;
    std::cout << "Agent events:    " << agent_events.size() << std::endl;
    std::cout << "Session events:  " << session_events.size() << std::endl;
    std::cout << "Dialogue events: " << dialogue_events.size() << std::endl;
    std::cout << "Task events:     " << task_events.size() << std::endl;
    std::cout << "Total events:    " << total_events << std::endl;

    std::cout << "\n--- Agent Events ---" << std::endl;
    for (const auto& e : agent_events) {
      std::cout << "  " << event_type_to_string(e.event.event_type)
                << " agent_id=" << e.event.agent_id << std::endl;
    }

    std::cout << "\n--- Session Events ---" << std::endl;
    for (const auto& e : session_events) {
      std::cout << "  " << event_type_to_string(e.event.event_type)
                << " session_id=" << e.event.session_id
                << " user_id=" << e.event.user_id << std::endl;
    }

    std::cout << "\n--- Dialogue Events ---" << std::endl;
    for (const auto& e : dialogue_events) {
      std::cout << "  " << event_type_to_string(e.event.event_type)
                << " dialogue_id=" << e.event.dialogue_id
                << " session_id=" << e.event.session_id << std::endl;
    }

    std::cout << "\n--- Task Events ---" << std::endl;
    for (const auto& e : task_events) {
      std::cout << "  " << event_type_to_string(e.event.event_type)
                << " task_id=" << e.event.task_id
                << " task_type=" << e.event.task_type
                << " skill_name=" << e.event.skill_name << std::endl;
    }
  }

private:
  static std::string event_type_to_string(runtime::EventType type) {
    switch (type) {
      case runtime::EventType::AGENT_REGISTERED:   return "AGENT_REGISTERED";
      case runtime::EventType::AGENT_UNREGISTERED: return "AGENT_UNREGISTERED";
      case runtime::EventType::SESSION_CREATED:    return "SESSION_CREATED";
      case runtime::EventType::SESSION_DESTROYED:  return "SESSION_DESTROYED";
      case runtime::EventType::DIALOGUE_CREATED:   return "DIALOGUE_CREATED";
      case runtime::EventType::DIALOGUE_DESTROYED: return "DIALOGUE_DESTROYED";
      case runtime::EventType::TASK_CREATED:       return "TASK_CREATED";
      case runtime::EventType::TASK_DESTROYED:     return "TASK_DESTROYED";
      default: return "UNKNOWN";
    }
  }
};

// =============================================================================
// Test Fixture
// =============================================================================

class EventHandlerBlackboxTest : public ::testing::Test {
protected:
  std::string find_agent_path() {
    std::vector<std::string> search_paths = {
        "examples/agent/system_agent_mock",
        "../examples/agent/system_agent_mock",
        "../../examples/agent/system_agent_mock",
        "../../../examples/agent/system_agent_mock",
        "/root/projects/AgentFlow/examples/agent/system_agent_mock"
    };

    for (const auto& path : search_paths) {
      std::filesystem::path full_path = std::filesystem::absolute(path);
      if (std::filesystem::exists(full_path / "package.json")) {
        return std::filesystem::canonical(full_path).string();
      }
    }
    return "";
  }

  void SetUp() override {
    // Find agent package
    package_path_ = find_agent_path();
    ASSERT_FALSE(package_path_.empty()) << "SystemAgentMock package not found";

    // Check required libraries
    std::vector<std::string> required_libs = {
        "lib/libskill_selection_node.so",
        "lib/libtravel_node.so",
        "lib/libshopping_node.so",
        "lib/libfoodie_node.so",
        "lib/libcoordinator_node.so"
    };
    for (const auto& lib : required_libs) {
      ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(package_path_) / lib))
          << "Required SO library not found: " << lib;
    }

    // Save original directory
    original_dir_ = std::filesystem::current_path().string();

    // Create event handler
    handler_ = std::make_shared<BlackboxEventHandler>();
  }

  void TearDown() override {
    if (runtime_) {
      runtime_->stop();
    }
    if (agent_) {
      agent_->stop();
    }
    std::filesystem::current_path(original_dir_);
  }

  std::string                      package_path_;
  std::string                      original_dir_;
  std::shared_ptr<BlackboxEventHandler> handler_;
  std::shared_ptr<framework::MMAgent>   agent_;
  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
};

// =============================================================================
// Test Cases
// =============================================================================

TEST_F(EventHandlerBlackboxTest, AgentLifecycleEvents) {
  std::cout << "\n=== Test: Agent Lifecycle Events ===" << std::endl;
  std::cout << "Package path: " << package_path_ << std::endl;

  // Change to package directory
  std::filesystem::current_path(package_path_);

  // Create runtime
  runtime::MMAgentRuntime::Config config;
  config.worker_threads = 2;
  runtime_ = std::make_unique<runtime::MMAgentRuntime>(config);

  // Register event handler BEFORE starting
  runtime_->register_event_handler(handler_);
  runtime_->set_callback_thread_pool_size(2);

  // Start runtime
  ASSERT_TRUE(runtime_->start()) << "Failed to start runtime";

  // Create agent from package
  agent_ = framework::MMAgent::create_from_package(".");
  ASSERT_NE(agent_, nullptr) << "Failed to create agent";

  std::string agent_id = agent_->get_id();
  std::cout << "Agent created: " << agent_->get_name() << " (id: " << agent_id << ")" << std::endl;

  // Initialize agent
  ASSERT_TRUE(agent_->initialize()) << "Failed to initialize agent";

  // Register agent - should emit AGENT_REGISTERED
  ASSERT_TRUE(runtime_->register_agent(agent_)) << "Failed to register agent";

  // Wait for AGENT_REGISTERED event
  bool got_event = handler_->wait_for_events(1, 5000);
  ASSERT_TRUE(got_event) << "Did not receive AGENT_REGISTERED event";

  // Verify event
  std::lock_guard<std::mutex> lock(handler_->events_mutex);
  ASSERT_EQ(handler_->agent_events.size(), 1u);
  EXPECT_EQ(handler_->agent_events[0].event.event_type, runtime::EventType::AGENT_REGISTERED);
  EXPECT_EQ(handler_->agent_events[0].event.agent_id, agent_id);
  EXPECT_EQ(handler_->agent_events[0].event.name, agent_->get_name());

  std::cout << "AGENT_REGISTERED event received for: " << agent_id << std::endl;
}

TEST_F(EventHandlerBlackboxTest, SessionLifecycleEvents) {
  std::cout << "\n=== Test: Session Lifecycle Events ===" << std::endl;

  // Change to package directory
  std::filesystem::current_path(package_path_);

  // Setup runtime and agent
  runtime::MMAgentRuntime::Config config;
  config.worker_threads = 2;
  runtime_ = std::make_unique<runtime::MMAgentRuntime>(config);
  runtime_->register_event_handler(handler_);
  runtime_->set_callback_thread_pool_size(2);
  ASSERT_TRUE(runtime_->start());

  agent_ = framework::MMAgent::create_from_package(".");
  ASSERT_NE(agent_, nullptr);
  ASSERT_TRUE(agent_->initialize());
  ASSERT_TRUE(runtime_->register_agent(agent_));
  ASSERT_TRUE(agent_->start());

  // Wait for AGENT_REGISTERED
  handler_->wait_for_events(1, 5000);
  handler_->total_events = 0;  // Reset counter

  // Create session via agent's public API
  std::string user_id = "test_user_session";
  std::string client_id = "test_client_session";
  auto session = agent_->get_or_create_session(user_id, client_id);
  ASSERT_NE(session, nullptr);

  std::string session_id = session->get_session_id();
  std::cout << "Session created: " << session_id << std::endl;

  // Wait for SESSION_CREATED event
  bool got_event = handler_->wait_for_events(1, 5000);
  ASSERT_TRUE(got_event) << "Did not receive SESSION_CREATED event";

  // Verify SESSION_CREATED
  {
    std::lock_guard<std::mutex> lock(handler_->events_mutex);
    bool found = false;
    for (const auto& e : handler_->session_events) {
      if (e.event.event_type == runtime::EventType::SESSION_CREATED &&
          e.event.session_id == session_id) {
        found = true;
        EXPECT_EQ(e.event.user_id, user_id);
        EXPECT_EQ(e.event.client_id, client_id);
        break;
      }
    }
    EXPECT_TRUE(found) << "SESSION_CREATED event not found for session " << session_id;
  }

  std::cout << "SESSION_CREATED event verified for: " << session_id << std::endl;
}

TEST_F(EventHandlerBlackboxTest, FullDialogueFlow) {
  std::cout << "\n=== Test: Full Dialogue Flow with Events ===" << std::endl;

  // Change to package directory
  std::filesystem::current_path(package_path_);

  // Setup runtime and agent
  runtime::MMAgentRuntime::Config config;
  config.worker_threads = 2;
  runtime_ = std::make_unique<runtime::MMAgentRuntime>(config);
  runtime_->register_event_handler(handler_);
  runtime_->set_callback_thread_pool_size(2);
  ASSERT_TRUE(runtime_->start());

  agent_ = framework::MMAgent::create_from_package(".");
  ASSERT_NE(agent_, nullptr);
  ASSERT_TRUE(agent_->initialize());
  ASSERT_TRUE(runtime_->register_agent(agent_));
  ASSERT_TRUE(agent_->start());

  // Wait for AGENT_REGISTERED
  handler_->wait_for_events(1, 5000);
  std::cout << "Agent registered, starting dialogue..." << std::endl;

  // Send a user message to trigger dialogue
  framework::MMAgentMsgQue::Message message;
  message.message_id = "blackbox-test-msg-001";
  message.metadata["message_type"] = "intent_request";
  message.metadata["user_id"] = "blackbox_user";
  message.metadata["client_id"] = "blackbox_client";
  message.content = "What are the best travel and food options?";

  ASSERT_TRUE(agent_->route_request_message(message)) << "Failed to route message";
  std::cout << "Message sent: " << message.content << std::endl;

  // Wait for response
  std::string response_content;
  bool got_response = false;
  const int max_wait_seconds = 30;

  for (int i = 0; i < max_wait_seconds * 10; ++i) {
    agent_->process_all_sessions_until_idle();

    framework::MMAgentMsgQue::Message response;
    while (agent_->dequeue_response_message(response)) {
      if (!response.content.empty()) {
        response_content = response.content;
        got_response = true;
        break;
      }
    }

    if (got_response) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  ASSERT_TRUE(got_response) << "No response received within " << max_wait_seconds << " seconds";
  std::cout << "Response received: " << response_content.substr(0, 100) << "..." << std::endl;

  // Wait a bit for all events to be processed
  handler_->wait_for_events(6, 3000);  // Expect at least: SESSION_CREATED, DIALOGUE_CREATED, TASK_CREATED, etc.

  // Print event summary
  handler_->print_summary();

  // Verify we got the expected events
  std::lock_guard<std::mutex> lock(handler_->events_mutex);

  // Should have SESSION_CREATED
  bool has_session_created = false;
  for (const auto& e : handler_->session_events) {
    if (e.event.event_type == runtime::EventType::SESSION_CREATED) {
      has_session_created = true;
      break;
    }
  }
  EXPECT_TRUE(has_session_created) << "Missing SESSION_CREATED event";

  // Should have DIALOGUE_CREATED
  bool has_dialogue_created = false;
  for (const auto& e : handler_->dialogue_events) {
    if (e.event.event_type == runtime::EventType::DIALOGUE_CREATED) {
      has_dialogue_created = true;
      break;
    }
  }
  EXPECT_TRUE(has_dialogue_created) << "Missing DIALOGUE_CREATED event";

  // Should have TASK_CREATED events (at least for dispatcher, skills, coordinator)
  int task_created_count = 0;
  for (const auto& e : handler_->task_events) {
    if (e.event.event_type == runtime::EventType::TASK_CREATED) {
      task_created_count++;
    }
  }
  EXPECT_GE(task_created_count, 1) << "Expected at least 1 TASK_CREATED event, got " << task_created_count;

  // Should have TASK_DESTROYED events
  int task_destroyed_count = 0;
  for (const auto& e : handler_->task_events) {
    if (e.event.event_type == runtime::EventType::TASK_DESTROYED) {
      task_destroyed_count++;
    }
  }
  EXPECT_GE(task_destroyed_count, 1) << "Expected at least 1 TASK_DESTROYED event";

  std::cout << "\n=== Black-box Test Summary ===" << std::endl;
  std::cout << "SESSION_CREATED: " << (has_session_created ? "OK" : "MISSING") << std::endl;
  std::cout << "DIALOGUE_CREATED: " << (has_dialogue_created ? "OK" : "MISSING") << std::endl;
  std::cout << "TASK_CREATED events: " << task_created_count << std::endl;
  std::cout << "TASK_DESTROYED events: " << task_destroyed_count << std::endl;
}

TEST_F(EventHandlerBlackboxTest, EventOrderConsistency) {
  std::cout << "\n=== Test: Event Order Consistency ===" << std::endl;

  // Change to package directory
  std::filesystem::current_path(package_path_);

  // Setup runtime and agent
  runtime::MMAgentRuntime::Config config;
  config.worker_threads = 2;
  runtime_ = std::make_unique<runtime::MMAgentRuntime>(config);
  runtime_->register_event_handler(handler_);
  runtime_->set_callback_thread_pool_size(2);
  ASSERT_TRUE(runtime_->start());

  agent_ = framework::MMAgent::create_from_package(".");
  ASSERT_NE(agent_, nullptr);
  ASSERT_TRUE(agent_->initialize());
  ASSERT_TRUE(runtime_->register_agent(agent_));
  ASSERT_TRUE(agent_->start());

  // Wait for AGENT_REGISTERED
  handler_->wait_for_events(1, 5000);

  // Send message
  framework::MMAgentMsgQue::Message message;
  message.message_id = "order-test-msg-001";
  message.metadata["message_type"] = "intent_request";
  message.metadata["user_id"] = "order_user";
  message.metadata["client_id"] = "order_client";
  message.content = "Test query for event ordering";

  ASSERT_TRUE(agent_->route_request_message(message));

  // Wait for response
  bool got_response = false;
  for (int i = 0; i < 300; ++i) {
    agent_->process_all_sessions_until_idle();
    framework::MMAgentMsgQue::Message response;
    while (agent_->dequeue_response_message(response)) {
      if (!response.content.empty()) {
        got_response = true;
        break;
      }
    }
    if (got_response) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  ASSERT_TRUE(got_response) << "No response received";

  // Wait for events
  handler_->wait_for_events(5, 3000);

  // Verify event ordering: AGENT_REGISTERED < SESSION_CREATED < DIALOGUE_CREATED
  std::lock_guard<std::mutex> lock(handler_->events_mutex);

  auto find_event_time = [](const auto& events, runtime::EventType type) ->
      std::chrono::steady_clock::time_point {
    for (const auto& e : events) {
      if (e.event.event_type == type) {
        return e.timestamp;
      }
    }
    return std::chrono::steady_clock::time_point{};
  };

  auto agent_registered_time = find_event_time(handler_->agent_events, runtime::EventType::AGENT_REGISTERED);
  auto session_created_time = find_event_time(handler_->session_events, runtime::EventType::SESSION_CREATED);
  auto dialogue_created_time = find_event_time(handler_->dialogue_events, runtime::EventType::DIALOGUE_CREATED);

  if (agent_registered_time.time_since_epoch().count() > 0 &&
      session_created_time.time_since_epoch().count() > 0) {
    EXPECT_LT(agent_registered_time, session_created_time)
        << "AGENT_REGISTERED should happen before SESSION_CREATED";
  }

  if (session_created_time.time_since_epoch().count() > 0 &&
      dialogue_created_time.time_since_epoch().count() > 0) {
    EXPECT_LT(session_created_time, dialogue_created_time)
        << "SESSION_CREATED should happen before DIALOGUE_CREATED";
  }

  std::cout << "Event ordering verified" << std::endl;
}

}  // namespace tests
}  // namespace agentflow