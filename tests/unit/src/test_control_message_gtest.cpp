// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Unit tests for CONTROL message type and session lifecycle management

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "agent/agent.h"
#include "agent/msgque.h"
#include "agent/session.h"
#include "agent/task.h"
#include "runtime/databus/databus.h"
#include "runtime/databus/local_databus.h"

namespace agentflow {
namespace tests {

namespace {
void write_workflow_json(const std::filesystem::path& path, const std::string& workflow_id) {
  std::ofstream wf(path);
  wf << R"({"id":")" << workflow_id << R"(","name":")" << workflow_id
     << R"(","version":"1.0.0","nodes":[{"id":"n1","name":"n1","type":"TASK"}],"edges":[]})";
}

std::shared_ptr<framework::MMAgentTask> find_task_by_type(
    const std::vector<std::shared_ptr<framework::MMAgentTask>>& tasks, const std::string& task_type) {
  for(const auto& task : tasks) {
    if(task && task->get_task_type() == task_type) {
      return task;
    }
  }
  return nullptr;
}
}  // namespace

class ControlMessageTest : public ::testing::Test {
 protected:
  framework::MMAgent::Config base_config() {
    framework::MMAgent::Config cfg;
    cfg.id = "control_test_agent";
    cfg.name = "control_test_agent";
    return cfg;
  }
};

// =============================================================================
// Message Queue Tests
// =============================================================================

TEST_F(ControlMessageTest, ControlMessageTypeRoutesToControlQueue) {
  framework::MMAgentMsgQue::Config qcfg;
  qcfg.max_queue_size = 10;
  framework::MMAgentMsgQue queue(qcfg);

  // Create a CONTROL message
  framework::MMAgentMsgQue::Message control_msg;
  control_msg.message_id = "control_001";
  control_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  control_msg.content = framework::MMAgentMsgQue::ControlCommand::SESSION_INTERRUPT;

  ASSERT_TRUE(queue.enqueue_message(control_msg));

  // Verify it's routed to CONTROL queue
  framework::MMAgentMsgQue::Message out;
  EXPECT_TRUE(queue.dequeue_message_by_type(framework::MMAgentMsgQue::MessageQueueType::CONTROL, out));
  EXPECT_EQ(out.message_id, "control_001");
  EXPECT_EQ(out.content, framework::MMAgentMsgQue::ControlCommand::SESSION_INTERRUPT);
}

TEST_F(ControlMessageTest, ControlQueueHasHighestPriority) {
  framework::MMAgentMsgQue::Config qcfg;
  qcfg.max_queue_size = 10;
  framework::MMAgentMsgQue queue(qcfg);

  // Enqueue messages in different queues
  framework::MMAgentMsgQue::Message intent_msg;
  intent_msg.message_id = "intent_001";
  intent_msg.type = framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
  intent_msg.metadata["message_type"] = "intent_request";
  ASSERT_TRUE(queue.enqueue_message(intent_msg));

  framework::MMAgentMsgQue::Message data_msg;
  data_msg.message_id = "data_001";
  data_msg.type = framework::MMAgentMsgQue::MessageType::TASK_RESPONSE;
  ASSERT_TRUE(queue.enqueue_message(data_msg));

  framework::MMAgentMsgQue::Message control_msg;
  control_msg.message_id = "control_001";
  control_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  control_msg.content = framework::MMAgentMsgQue::ControlCommand::SESSION_DESTROY;
  ASSERT_TRUE(queue.enqueue_message(control_msg));

  // dequeue_message should return CONTROL first (highest priority)
  framework::MMAgentMsgQue::Message out;
  ASSERT_TRUE(queue.dequeue_message(out));
  EXPECT_EQ(out.message_id, "control_001");
}

TEST_F(ControlMessageTest, ControlQueueSizeIsTracked) {
  framework::MMAgentMsgQue::Config qcfg;
  framework::MMAgentMsgQue queue(qcfg);

  EXPECT_EQ(queue.get_queue_size(framework::MMAgentMsgQue::MessageQueueType::CONTROL), 0);

  framework::MMAgentMsgQue::Message msg;
  msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  queue.enqueue_message(msg);

  EXPECT_EQ(queue.get_queue_size(framework::MMAgentMsgQue::MessageQueueType::CONTROL), 1);

  framework::MMAgentMsgQue::Message out;
  queue.dequeue_message(out);

  EXPECT_EQ(queue.get_queue_size(framework::MMAgentMsgQue::MessageQueueType::CONTROL), 0);
}

// =============================================================================
// Task State Tests
// =============================================================================

TEST_F(ControlMessageTest, TaskInterruptSetsInterruptedState) {
  framework::MMAgentTask::Config task_cfg;
  task_cfg.task_id = "test_task_001";
  task_cfg.task_type = "skill";

  auto task = std::make_shared<framework::MMAgentTask>(task_cfg);

  // Start the task
  ASSERT_TRUE(task->start());
  EXPECT_EQ(task->get_state(), framework::MMAgentTask::TaskState::RUNNING);

  // Interrupt the task
  EXPECT_TRUE(task->interrupt());
  EXPECT_EQ(task->get_state(), framework::MMAgentTask::TaskState::INTERRUPTED);
}

TEST_F(ControlMessageTest, InterruptFailsForCompletedTask) {
  framework::MMAgentTask::Config task_cfg;
  task_cfg.task_id = "test_task_002";
  task_cfg.task_type = "skill";

  auto task = std::make_shared<framework::MMAgentTask>(task_cfg);
  task->start();
  task->complete("done");

  // Cannot interrupt a completed task
  EXPECT_FALSE(task->interrupt());
  EXPECT_EQ(task->get_state(), framework::MMAgentTask::TaskState::COMPLETED);
}

TEST_F(ControlMessageTest, InterruptFailsForCancelledTask) {
  framework::MMAgentTask::Config task_cfg;
  task_cfg.task_id = "test_task_003";
  task_cfg.task_type = "skill";

  auto task = std::make_shared<framework::MMAgentTask>(task_cfg);
  task->start();
  task->cancel();

  // Cannot interrupt a cancelled task
  EXPECT_FALSE(task->interrupt());
  EXPECT_EQ(task->get_state(), framework::MMAgentTask::TaskState::CANCELLED);
}

TEST_F(ControlMessageTest, InterruptedStateIsDifferentFromCancelled) {
  framework::MMAgentTask::Config task_cfg;
  task_cfg.task_id = "test_task_004";
  task_cfg.task_type = "skill";

  auto task_interrupted = std::make_shared<framework::MMAgentTask>(task_cfg);
  task_interrupted->start();
  task_interrupted->interrupt();

  // Create a second task with different ID for cancelled test
  framework::MMAgentTask::Config task_cfg2;
  task_cfg2.task_id = "test_task_005";
  task_cfg2.task_type = "skill";
  auto task_cancelled = std::make_shared<framework::MMAgentTask>(task_cfg2);
  task_cancelled->start();
  task_cancelled->cancel();

  // INTERRUPTED and CANCELLED are different states
  EXPECT_NE(task_interrupted->get_state(), task_cancelled->get_state());
  EXPECT_EQ(task_interrupted->get_state(), framework::MMAgentTask::TaskState::INTERRUPTED);
  EXPECT_EQ(task_cancelled->get_state(), framework::MMAgentTask::TaskState::CANCELLED);
}

// =============================================================================
// DataBus remove_by_prefix Tests
// =============================================================================

TEST_F(ControlMessageTest, DataBusRemoveByPrefixClearsMatchingKeys) {
  runtime::MMDataBus databus;

  // Add keys with different prefixes
  databus.put("/session/test/key1", std::string("value1"));
  databus.put("/session/test/key2", std::string("value2"));
  databus.put("/session/other/key3", std::string("value3"));

  // Verify all keys exist
  EXPECT_TRUE(databus.contains("/session/test/key1"));
  EXPECT_TRUE(databus.contains("/session/test/key2"));
  EXPECT_TRUE(databus.contains("/session/other/key3"));

  // Remove by prefix
  int removed = databus.remove_by_prefix("/session/test");
  EXPECT_EQ(removed, 2);

  // Verify only matching keys removed
  EXPECT_FALSE(databus.contains("/session/test/key1"));
  EXPECT_FALSE(databus.contains("/session/test/key2"));
  EXPECT_TRUE(databus.contains("/session/other/key3"));
}

TEST_F(ControlMessageTest, DataBusRemoveByPrefixReturnsZeroForNoMatches) {
  runtime::MMDataBus databus;

  databus.put("/session/abc/key1", std::string("value1"));

  int removed = databus.remove_by_prefix("/session/nonexistent");
  EXPECT_EQ(removed, 0);

  // Original key still exists
  EXPECT_TRUE(databus.contains("/session/abc/key1"));
}

TEST_F(ControlMessageTest, DataBusListKeysByPrefixReturnsMatchingKeys) {
  runtime::MMDataBus databus;

  databus.put("/session/test/key1", std::string("value1"));
  databus.put("/session/test/key2", std::string("value2"));
  databus.put("/session/other/key3", std::string("value3"));

  auto keys = databus.list_keys_by_prefix("/session/test");
  EXPECT_EQ(keys.size(), 2u);

  // Keys should contain the matching ones
  bool found_key1 = false, found_key2 = false;
  for(const auto& key : keys) {
    if(key == "/session/test/key1") found_key1 = true;
    if(key == "/session/test/key2") found_key2 = true;
  }
  EXPECT_TRUE(found_key1);
  EXPECT_TRUE(found_key2);
}

// =============================================================================
// Session clear_all_tasks Tests
// =============================================================================

TEST_F(ControlMessageTest, SessionClearAllTasksRemovesAllTasks) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  // Create a session
  auto session = agent->get_or_create_session("user_clear_test", "client_clear_test");
  ASSERT_NE(session, nullptr);

  // Create dispatcher and coordinator tasks
  framework::MMAgentTask::Config dispatcher_cfg;
  dispatcher_cfg.task_id = "dispatcher_task";
  dispatcher_cfg.task_type = "dispatcher";
  auto dispatcher_task = std::make_shared<framework::MMAgentTask>(dispatcher_cfg);
  session->set_dispatcher_task(dispatcher_task);

  framework::MMAgentTask::Config coordinator_cfg;
  coordinator_cfg.task_id = "coordinator_task";
  coordinator_cfg.task_type = "coordinator";
  auto coordinator_task = std::make_shared<framework::MMAgentTask>(coordinator_cfg);
  session->set_coordinator_task(coordinator_task);

  // Add a skill task
  framework::MMAgentTask::Config skill_cfg;
  skill_cfg.task_id = "skill_task";
  skill_cfg.task_type = "skill";
  auto skill_task = std::make_shared<framework::MMAgentTask>(skill_cfg);
  session->add_task(skill_task);

  // Verify tasks exist
  EXPECT_NE(session->get_dispatcher_task(), nullptr);
  EXPECT_NE(session->get_coordinator_task(), nullptr);
  // Note: get_tasks() may return more tasks from session pool template tasks

  // Clear all tasks
  session->clear_all_tasks();

  // Verify all tasks cleared
  EXPECT_EQ(session->get_dispatcher_task(), nullptr);
  EXPECT_EQ(session->get_coordinator_task(), nullptr);
  EXPECT_EQ(session->get_tasks().size(), 0u);
}

// =============================================================================
// Control Command Constants Tests
// =============================================================================

TEST_F(ControlMessageTest, ControlCommandConstantsAreCorrect) {
  EXPECT_STREQ(framework::MMAgentMsgQue::ControlCommand::SESSION_DESTROY, "SESSION-DESTROY");
  EXPECT_STREQ(framework::MMAgentMsgQue::ControlCommand::SESSION_INTERRUPT, "SESSION-INTERRUPT");
}

TEST_F(ControlMessageTest, ControlMetadataConstantsAreCorrect) {
  EXPECT_STREQ(framework::MMAgentMsgQue::ControlMetadata::USER_ID, "user_id");
  EXPECT_STREQ(framework::MMAgentMsgQue::ControlMetadata::CLIENT_ID, "client_id");
  EXPECT_STREQ(framework::MMAgentMsgQue::ControlMetadata::SESSION_ID, "session_id");
}

// =============================================================================
// Integration Tests
// =============================================================================

TEST_F(ControlMessageTest, SessionInterruptStopsRunningSkillTasks) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_control_interrupt_pkg");
  fs::create_directories(tmp);

  // Create minimal agent package with skill
  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "control-interrupt-agent",
      "version": "1.0.0",
      "workflows": ["workflow_main.json", "workflow_skill.json"]
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "sessions": [
        {
          "id": 0,
          "description": "Test session",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "workflow_main.json" },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "workflow_main.json" },
            {
              "id": 2,
              "type": "skill",
              "description": "Test skill",
              "workflow_file": "workflow_skill.json",
              "skill": { "name": "test-skill", "config_dir": "skills/test-skill" }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow_main.json", "wf_main");
  write_workflow_json(tmp / "workflow_skill.json", "wf_skill");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);
  ASSERT_TRUE(agent->initialize());

  // Get session
  auto sessions = agent->get_sessions();
  ASSERT_FALSE(sessions.empty());
  auto session = sessions.front();
  ASSERT_NE(session, nullptr);

  // Create a skill task and add it to session
  framework::MMAgentTask::Config skill_cfg;
  skill_cfg.task_id = "skill_running_test";
  skill_cfg.task_type = "skill";
  auto skill_task = std::make_shared<framework::MMAgentTask>(skill_cfg);
  skill_task->start();  // Set to RUNNING state
  session->add_task(skill_task);

  // Verify skill is running
  EXPECT_EQ(skill_task->get_state(), framework::MMAgentTask::TaskState::RUNNING);

  // Send SESSION-INTERRUPT control message
  framework::MMAgentMsgQue::Message interrupt_msg;
  interrupt_msg.message_id = "interrupt_001";
  interrupt_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  interrupt_msg.content = framework::MMAgentMsgQue::ControlCommand::SESSION_INTERRUPT;
  interrupt_msg.metadata["session_id"] = session->get_session_id();

  agent->route_request_message(interrupt_msg);

  // Give time for processing
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Verify skill task is interrupted
  EXPECT_EQ(skill_task->get_state(), framework::MMAgentTask::TaskState::INTERRUPTED);

  fs::remove_all(tmp);
}

TEST_F(ControlMessageTest, SessionDestroyClearsSessionData) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  // Create a session
  auto session = agent->get_or_create_session("user_destroy_test", "client_destroy_test");
  ASSERT_NE(session, nullptr);

  std::string session_id = session->get_session_id();

  // Add data to agent's DataBus under session prefix
  agent->publish_data("/session/" + session_id + "/key1", std::string("value1"));
  agent->publish_data("/session/" + session_id + "/key2", std::string("value2"));

  // Verify data exists
  DataVariant val;
  EXPECT_TRUE(agent->get_data("/session/" + session_id + "/key1", val));
  EXPECT_TRUE(agent->get_data("/session/" + session_id + "/key2", val));

  // Create and add tasks to session
  framework::MMAgentTask::Config dispatcher_cfg;
  dispatcher_cfg.task_id = "destroy_dispatcher";
  dispatcher_cfg.task_type = "dispatcher";
  auto dispatcher_task = std::make_shared<framework::MMAgentTask>(dispatcher_cfg);
  dispatcher_task->start();
  session->set_dispatcher_task(dispatcher_task);

  framework::MMAgentTask::Config skill_cfg;
  skill_cfg.task_id = "destroy_skill";
  skill_cfg.task_type = "skill";
  auto skill_task = std::make_shared<framework::MMAgentTask>(skill_cfg);
  skill_task->start();
  session->add_task(skill_task);

  // Send SESSION-DESTROY control message
  framework::MMAgentMsgQue::Message destroy_msg;
  destroy_msg.message_id = "destroy_001";
  destroy_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  destroy_msg.content = framework::MMAgentMsgQue::ControlCommand::SESSION_DESTROY;
  destroy_msg.metadata["session_id"] = session_id;

  agent->route_request_message(destroy_msg);

  // Verify session data cleared from DataBus
  DataVariant val_after;
  EXPECT_FALSE(agent->get_data("/session/" + session_id + "/key1", val_after));
  EXPECT_FALSE(agent->get_data("/session/" + session_id + "/key2", val_after));

  // Verify tasks cleared from session
  EXPECT_EQ(session->get_dispatcher_task(), nullptr);
  EXPECT_EQ(session->get_tasks().size(), 0u);
}

TEST_F(ControlMessageTest, UnknownControlCommandLogsWarning) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  auto session = agent->get_or_create_session("user_unknown_cmd", "client_unknown_cmd");
  ASSERT_NE(session, nullptr);

  // Send an unknown control command
  framework::MMAgentMsgQue::Message unknown_msg;
  unknown_msg.message_id = "unknown_001";
  unknown_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  unknown_msg.content = "UNKNOWN-COMMAND";
  unknown_msg.metadata["session_id"] = session->get_session_id();

  // Should not crash, just log a warning
  EXPECT_NO_THROW(agent->route_request_message(unknown_msg));
}

TEST_F(ControlMessageTest, ControlMessageWithoutSessionDoesNotCrash) {
  framework::MMAgentMsgQue::Config qcfg;
  framework::MMAgentMsgQue queue(qcfg);

  // CONTROL message without any session routing
  framework::MMAgentMsgQue::Message control_msg;
  control_msg.message_id = "no_session";
  control_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  control_msg.content = framework::MMAgentMsgQue::ControlCommand::SESSION_INTERRUPT;

  // Should enqueue successfully
  EXPECT_TRUE(queue.enqueue_message(control_msg));
}

}  // namespace tests
}  // namespace agentflow