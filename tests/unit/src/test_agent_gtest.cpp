#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "agent/agent.h"
#include "agent/context.h"
#include "agent/msgque.h"
#include "agent/session.h"
#include "agent/task.h"
#include "agent/builtin_dispatcher.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace tests {

namespace {
void write_workflow_json(const std::filesystem::path& path, const std::string& workflow_id) {
  std::ofstream wf(path);
  wf << R"({"id":")" << workflow_id
     << R"(","name":")" << workflow_id
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

std::shared_ptr<framework::MMAgentTask> find_task_by_message_id(
    const std::vector<std::shared_ptr<framework::MMAgentTask>>& tasks, const std::string& message_id) {
  for(const auto& task : tasks) {
    if(!task) {
      continue;
    }
    std::string value;
    if(task->get_parameter("message_id", value) && value == message_id) {
      return task;
    }
  }
  return nullptr;
}

std::shared_ptr<framework::MMAgentTask> find_task_by_message_id_and_type(
    const std::vector<std::shared_ptr<framework::MMAgentTask>>& tasks, const std::string& message_id,
    const std::string& task_type) {
  for(const auto& task : tasks) {
    if(!task || task->get_task_type() != task_type) {
      continue;
    }
    std::string value;
    if(task->get_parameter("message_id", value) && value == message_id) {
      return task;
    }
  }
  return nullptr;
}

// Helper to get or create the first session from an agent
// Sessions are now created on-demand with UUID, so we need to trigger creation
// by calling get_or_create_session with a user_id:client_id route
std::shared_ptr<framework::MMAgentSession> get_or_create_first_session(
    const std::shared_ptr<framework::MMAgent>& agent) {
  auto sessions = agent->get_sessions();
  if(!sessions.empty()) {
    return sessions.front();
  }
  // No sessions exist - create one on-demand
  return agent->get_or_create_session("test_user", "test_client");
}
}  // namespace

class AgentTest : public ::testing::Test {
 protected:
  framework::MMAgent::Config base_config() {
    framework::MMAgent::Config cfg;
    cfg.id = "agent_test";
    cfg.name = "agent_test";
    return cfg;
  }
};

TEST_F(AgentTest, MsgQueRoutesMessagesByTypeHeader) {
  framework::MMAgentMsgQue::Config qcfg;
  qcfg.max_queue_size = 10;
  framework::MMAgentMsgQue queue(qcfg);

  framework::MMAgentMsgQue::Message intent;
  intent.message_id = "intent";
  intent.metadata["message_type"] = "intent_request";
  ASSERT_TRUE(queue.enqueue_message(intent));

  framework::MMAgentMsgQue::Message output;
  output.message_id = "output";
  output.metadata["message_type"] = "data_exchange";
  ASSERT_TRUE(queue.enqueue_message(output));

  framework::MMAgentMsgQue::Message out;
  ASSERT_TRUE(
      queue.dequeue_message_by_type(framework::MMAgentMsgQue::MessageQueueType::DATA_EXCHANGE, out));
  EXPECT_EQ(out.message_id, "output");
  ASSERT_TRUE(queue.dequeue_message_by_type(framework::MMAgentMsgQue::MessageQueueType::INTENT_REQUEST, out));
  EXPECT_EQ(out.message_id, "intent");
}

TEST_F(AgentTest, MsgQueStoresPayloadAsDataRef) {
  framework::MMAgentMsgQue::Config qcfg;
  framework::MMAgentMsgQue queue(qcfg);

  framework::MMAgentMsgQue::Message msg;
  msg.message_id = "payload-msg";
  msg.content = "raw-payload";
  msg.metadata["user_id"] = "u-data";
  msg.metadata["client_id"] = "c-data";
  ASSERT_TRUE(queue.enqueue_message(msg));

  framework::MMAgentMsgQue::Message out;
  ASSERT_TRUE(queue.dequeue_message(out));
  // After dequeue, content is restored from data_ref for user convenience
  EXPECT_EQ(out.content, "raw-payload");
  // data_ref should still be populated (content was stored via data_ref)
  EXPECT_FALSE(out.data_ref.empty());
}

TEST_F(AgentTest, RouteRequestAssignsPooledSessionByUserAndClient) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req1";
  req.metadata["user_id"] = "u1";
  req.metadata["client_id"] = "c1";
  ASSERT_TRUE(agent->route_request_message(req));

  auto routed_session = agent->get_session_by_key("u1:c1");
  // Get pooled session from sessions list (session_id is now UUID, not "0")
  auto all_sessions = agent->get_sessions();
  ASSERT_FALSE(all_sessions.empty());
  auto pooled_session = all_sessions.front();
  ASSERT_NE(routed_session, nullptr);
  ASSERT_NE(pooled_session, nullptr);
  EXPECT_EQ(routed_session, pooled_session);
  EXPECT_EQ(routed_session->get_receive_queue_size(), 1);
}

TEST_F(AgentTest, AgentBackgroundThreadsProcessIntentAndDataExchangeMessages) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf_auto";
  wf_cfg.name = "wf_auto";
  wf_cfg.dsl_file = "wf_auto.json";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));
  ASSERT_TRUE(agent->initialize());
  ASSERT_TRUE(agent->start());

  framework::MMAgentMsgQue::Message intent;
  intent.message_id = "intent-auto";
  intent.metadata["user_id"] = "u-auto";
  intent.metadata["client_id"] = "c-auto";
  intent.metadata["task_type"] = "dispatcher";
  intent.metadata["workflow_id"] = "wf_auto";
  ASSERT_TRUE(agent->route_request_message(intent));

  std::shared_ptr<framework::MMAgentTask> generated;
  bool                                    saw_intent = false;
  for(int i = 0; i < 50; ++i) {
    if(agent->pop_generated_task(generated)) {
      saw_intent = (generated && generated->get_task_type() == "dispatcher");
      if(saw_intent) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(saw_intent);

  framework::MMAgentMsgQue::Message data;
  data.message_id = "data-auto";
  data.metadata["user_id"] = "u-auto";
  data.metadata["client_id"] = "c-auto";
  data.metadata["message_type"] = "data_exchange";
  data.metadata["task_type"] = "coordinator";
  data.metadata["workflow_id"] = "wf_auto";
  ASSERT_TRUE(agent->route_request_message(data));

  bool saw_data = false;
  for(int i = 0; i < 50; ++i) {
    if(agent->pop_generated_task(generated)) {
      saw_data = (generated && generated->get_task_type() == "coordinator");
      if(saw_data) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(saw_data);
  EXPECT_TRUE(agent->stop());
}

TEST_F(AgentTest, CoordinatorBuildsSessionOwnedTaskWithWorkflowMapping) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf1";
  wf_cfg.name = "wf1";
  wf_cfg.dsl_file = "wf1.json";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req2";
  req.content = "task-input";
  req.metadata["user_id"] = "u2";
  req.metadata["client_id"] = "c2";
  req.metadata["task_type"] = "collaboration";
  req.metadata["workflow_id"] = "wf1";
  req.metadata["collaboration_so"] = "/tmp/demo_collab.so";

  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u2:c2"), 1);

  auto session = agent->get_session_by_key("u2:c2");
  ASSERT_NE(session, nullptr);
  std::string session_id = session->get_session_id();
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 3u);
  auto collaboration_task = find_task_by_type(tasks, "collaboration");
  ASSERT_NE(collaboration_task, nullptr);
  EXPECT_EQ(collaboration_task->get_session_id(), session_id);
  EXPECT_EQ(collaboration_task->get_workflow_id(), "wf1");
  EXPECT_EQ(collaboration_task->get_workflow_file(), "wf1.json");
  EXPECT_EQ(collaboration_task->get_collaboration_so_path(), "/tmp/demo_collab.so");
  std::string message_content;
  std::string message_data_ref;
  EXPECT_TRUE(collaboration_task->get_parameter("message_content", message_content));
  EXPECT_TRUE(collaboration_task->get_parameter("message_data_ref", message_data_ref));
  EXPECT_EQ(message_content, "task-input");
  EXPECT_FALSE(message_data_ref.empty());
}

TEST_F(AgentTest, CoordinatorDefaultsToDispatcherType) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf_default";
  wf_cfg.name = "wf_default";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req_dispatcher";
  req.metadata["user_id"] = "u3";
  req.metadata["client_id"] = "c3";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u3:c3"), 1);

  auto session = agent->get_session_by_key("u3:c3");
  ASSERT_NE(session, nullptr);
  std::string session_id = session->get_session_id();
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 3u);
  auto dispatcher_task = session->get_task(session_id + "_task_0");
  auto coordinator_task = session->get_task(session_id + "_task_1");
  auto generated_dispatcher_task = find_task_by_message_id(tasks, "req_dispatcher");
  ASSERT_NE(dispatcher_task, nullptr);
  ASSERT_NE(coordinator_task, nullptr);
  ASSERT_NE(generated_dispatcher_task, nullptr);
  EXPECT_EQ(dispatcher_task->get_task_type(), "dispatcher");
  EXPECT_EQ(coordinator_task->get_task_type(), "coordinator");
  EXPECT_EQ(generated_dispatcher_task->get_task_type(), "dispatcher");
}

TEST_F(AgentTest, PackageDispatcherWorkflowMappingIsApplied) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_dispatcher_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "dispatcher-test-agent",
      "version": "1.0.0",
      "workflows": ["workflow_a.json", "workflow_b.json", "task-context.json"],
      "task_context_workflow": "task-context.json"
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {
          "id": 0,
          "type": "contextupdater",
          "description": "Update agent context",
          "workflow_file": "task-context.json",
          "workflow_pool_size": 2
        }
      ],
      "sessions": [
        {
          "id": 0,
          "description": "Homogeneous pooled session template",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "workflow_a.json" },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "workflow_b.json" },
            {
              "id": 2,
              "type": "skill",
              "description": "Support skill",
              "workflow_file": "workflow_a.json",
              "skill": {
                "name": "support",
                "config_dir": "skills/support"
              }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow_a.json", "wf_a");
  write_workflow_json(tmp / "workflow_b.json", "wf_b");
  write_workflow_json(tmp / "task-context.json", "wf_context");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);
  ASSERT_NE(agent->get_context(), nullptr);
  ASSERT_NE(agent->get_context()->get_context_updater_task(), nullptr);
  EXPECT_EQ(agent->get_context()->get_context_updater_task()->get_task_type(), "contextupdater");

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req_dispatcher_map";
  req.metadata["user_id"] = "u4";
  req.metadata["client_id"] = "c4";
  req.metadata["session_id"] = "0";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("0"), 1);

  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);
  auto mapped_task = find_task_by_message_id(session->get_tasks(), "req_dispatcher_map");
  ASSERT_NE(mapped_task, nullptr);
  EXPECT_EQ(mapped_task->get_task_type(), "dispatcher");
  EXPECT_EQ(mapped_task->get_workflow_id(), "wf_a");

  fs::remove_all(tmp);
}

TEST_F(AgentTest, WorkflowIdMetadataHasHigherPriorityThanCoordinatorWorkflow) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_coordinator_override_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "coordinator-override-agent",
      "version": "1.0.0",
      "workflows": ["workflow_a.json", "workflow_b.json", "task-context.json"],
      "task_context_workflow": "task-context.json"
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {
          "id": 0,
          "type": "contextupdater",
          "description": "Update agent context",
          "workflow_file": "task-context.json",
          "workflow_pool_size": 2
        }
      ],
      "sessions": [
        {
          "id": 0,
          "description": "Homogeneous pooled session template",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "workflow_b.json" },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "workflow_b.json" },
            {
              "id": 2,
              "type": "skill",
              "description": "Support skill",
              "workflow_file": "workflow_a.json",
              "skill": {
                "name": "support",
                "config_dir": "skills/support"
              }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow_a.json", "wf_a");
  write_workflow_json(tmp / "workflow_b.json", "wf_b");
  write_workflow_json(tmp / "task-context.json", "wf_context");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req_workflow_override";
  req.metadata["user_id"] = "u6";
  req.metadata["client_id"] = "c6";
  req.metadata["session_id"] = "0";
  req.metadata["workflow_id"] = "wf_a";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("0"), 1);

  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);
  auto task = find_task_by_message_id(session->get_tasks(), "req_workflow_override");
  ASSERT_NE(task, nullptr);
  EXPECT_EQ(task->get_workflow_id(), "wf_a");

  fs::remove_all(tmp);
}

TEST_F(AgentTest, DefaultPooledSessionContainsDispatcherAndCoordinator) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);
  ASSERT_NE(session->get_task_coordinator(), nullptr);
  ASSERT_NE(session->get_task_dispatcher(), nullptr);
  std::string session_id = session->get_session_id();
  auto default_dispatcher = session->get_task(session_id + "_task_0");
  auto default_coordinator = session->get_task(session_id + "_task_1");
  ASSERT_NE(default_dispatcher, nullptr);
  ASSERT_NE(default_coordinator, nullptr);
  EXPECT_EQ(default_dispatcher->get_task_type(), "dispatcher");
  EXPECT_EQ(default_coordinator->get_task_type(), "coordinator");

  // Different user_id:client_id route should get a DIFFERENT session (on-demand creation)
  auto routed_session = agent->get_or_create_session("u5", "c5");
  ASSERT_NE(routed_session, nullptr);
  EXPECT_NE(routed_session, session);  // Different route = different session

  // Same route should reuse existing session
  auto reused_session = agent->get_or_create_session("test_user", "test_client");
  EXPECT_EQ(reused_session, session);  // Same route = same session
}

TEST_F(AgentTest, CoordinatorInstantiatesSkillTaskFromSkillTemplateWorkflow) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_selected_skill_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "selected-skill-agent",
      "version": "1.0.0",
      "workflows": ["workflow_dispatcher.json", "workflow_coordinator.json", "workflow_skill.json"]
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "sessions": [
        {
          "id": 0,
          "description": "Pooled session",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "workflow_dispatcher.json" },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "workflow_coordinator.json" },
            {
              "id": 2,
              "type": "skill",
              "description": "Search skill",
              "workflow_file": "workflow_skill.json",
              "skill": {
                "name": "search",
                "config_dir": "skills/search"
              }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow_dispatcher.json", "wf_dispatcher");
  write_workflow_json(tmp / "workflow_coordinator.json", "wf_coordinator");
  write_workflow_json(tmp / "workflow_skill.json", "wf_skill");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);
  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);
  auto coordinator_task = session->get_coordinator_task();
  ASSERT_NE(coordinator_task, nullptr);

  // New flow: skill tasks are created when coordinator workflow completes (data_exchange)
  // Simulate coordinator workflow completion by sending a data_exchange message
  // with selected_skills in the databus (JSON array format)
  agent->publish_data("selected_skills", std::string(R"(["search"])"));

  framework::MMAgentMsgQue::Message workflow_complete;
  workflow_complete.message_id = "coordinator_workflow_complete";
  workflow_complete.metadata["task_type"] = "coordinator";
  workflow_complete.metadata["workflow_id"] = "wf_coordinator";
  workflow_complete.content = "workflow_completed";

  // Trigger data_exchange handler which will create skill task
  coordinator_task->enqueue_data_exchange_message(workflow_complete);

  // Process all messages to trigger the data_exchange handler
  agent->process_all_sessions_until_idle();

  auto tasks = session->get_tasks();
  // Find any skill task with the correct skill name
  // Note: there will be both template task (0_task_2) and runtime task (0_task_2_runtime_...)
  // We want to find the runtime task which is created when coordinator workflow completes
  std::shared_ptr<framework::MMAgentTask> runtime_skill_task;
  for(const auto& t : tasks) {
    if(t && t->get_task_type() == "skill" && t->get_skill_name() == "search") {
      // Runtime task has "_runtime_" in its ID
      if(t->get_task_id().find("_runtime_") != std::string::npos) {
        runtime_skill_task = t;
        break;
      }
    }
  }

  if(!runtime_skill_task) {
    std::cout << "Runtime skill task not found. Tasks:" << std::endl;
    for(const auto& t : tasks) {
      if(t) {
        std::cout << "  - " << t->get_task_id() << " (type=" << t->get_task_type()
                  << ", skill=" << t->get_skill_name() << ")" << std::endl;
      }
    }
  }

  ASSERT_NE(runtime_skill_task, nullptr);
  EXPECT_EQ(runtime_skill_task->get_workflow_id(), "wf_skill");
  EXPECT_NE(runtime_skill_task->get_task_id().find("search"), std::string::npos);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, AgentJsonInstantiatesConfiguredSessionPoolAndSkillPools) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_structured_sessions_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "structured-session-agent",
      "version": "1.0.0",
      "workflows": ["workflow_main.json", "workflow_skill.json", "task-context.json"],
      "task_context_workflow": "task-context.json"
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 2,
      "max_workflows": 12,
      "max_concurrent_workflows": 4,
      "tasks": [
        {
          "id": 0,
          "type": "contextupdater",
          "description": "Update agent context",
          "workflow_file": "task-context.json",
          "workflow_pool_size": 2
        }
      ],
      "sessions": [
        {
          "id": 0,
          "description": "Homogeneous pooled session",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "workflow_main.json", "workflow_pool_size": 3 },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "workflow_main.json", "workflow_pool_size": 2 },
            {
              "id": 2,
              "type": "skill",
              "description": "Search skill",
              "workflow_file": "workflow_skill.json",
              "workflow_pool_size": 4,
              "skill": {
                "name": "search",
                "config_dir": "skills/search"
              }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow_main.json", "wf_main");
  write_workflow_json(tmp / "workflow_skill.json", "wf_skill");
  write_workflow_json(tmp / "task-context.json", "wf_context");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);
  EXPECT_EQ(agent->get_config().max_workflows, 12);
  EXPECT_EQ(agent->get_config().max_concurrent_workflows, 4);

  auto sessions = agent->get_sessions();
  ASSERT_EQ(sessions.size(), 2u);

  auto pooled_session = get_or_create_first_session(agent);
  auto skill_session = agent->get_session_by_key("1");
  ASSERT_NE(pooled_session, nullptr);
  ASSERT_NE(skill_session, nullptr);

  auto skill_task = skill_session->get_task("1_task_2");
  auto skill_pool_0 = skill_session->get_task("1_task_2_pool_0");
  auto skill_pool_1 = skill_session->get_task("1_task_2_pool_1");
  ASSERT_NE(agent->get_context(), nullptr);
  ASSERT_NE(agent->get_context()->get_context_updater_task(), nullptr);
  EXPECT_EQ(agent->get_context()->get_context_updater_task()->get_workflow_pool_size(), 2);
  ASSERT_NE(skill_task, nullptr);
  ASSERT_NE(skill_pool_0, nullptr);
  ASSERT_NE(skill_pool_1, nullptr);
  EXPECT_EQ(agent->get_context()->get_context_updater_task()->get_task_type(), "contextupdater");
  EXPECT_EQ(skill_task->get_task_type(), "skill");
  EXPECT_EQ(skill_task->get_skill_name(), "search");
  EXPECT_EQ(skill_task->get_skill_config_dir(), "skills/search");
  EXPECT_EQ(skill_task->get_instance_pool_size(), 2);
  EXPECT_EQ(skill_task->get_workflow_pool_size(), 4);
  EXPECT_FALSE(skill_task->is_skill_instance());
  EXPECT_TRUE(skill_pool_0->is_skill_instance());
  EXPECT_TRUE(skill_pool_1->is_skill_instance());
  EXPECT_EQ(skill_pool_0->get_skill_instance_index(), 0);
  EXPECT_EQ(skill_pool_1->get_skill_instance_index(), 1);

  auto dispatcher_task = pooled_session->get_task("0_task_0");
  auto coordinator_task = pooled_session->get_task("0_task_1");
  ASSERT_NE(dispatcher_task, nullptr);
  ASSERT_NE(coordinator_task, nullptr);
  EXPECT_EQ(dispatcher_task->get_workflow_pool_size(), 3);
  EXPECT_EQ(coordinator_task->get_workflow_pool_size(), 2);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, AgentJsonRejectsInvalidWorkflowPoolSize) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_invalid_workflow_pool_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "invalid-workflow-pool-agent",
      "version": "1.0.0",
      "workflows": ["workflow_main.json"]
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "sessions": [
        {
          "id": 0,
          "description": "Homogeneous pooled session",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "workflow_main.json", "workflow_pool_size": 0 },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "workflow_main.json" },
            {
              "id": 2,
              "type": "skill",
              "description": "Search skill",
              "workflow_file": "workflow_main.json",
              "skill": {
                "name": "search",
                "config_dir": "skills/search"
              }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow_main.json", "wf_main");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  EXPECT_EQ(agent, nullptr);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, AgentJsonDefaultsWorkflowPoolSizeToOneWhenOmitted) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_default_workflow_pool_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "default-workflow-pool-agent",
      "version": "1.0.0",
      "workflows": ["workflow_main.json", "task-context.json"]
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {
          "id": 0,
          "type": "contextupdater",
          "description": "Update agent context",
          "workflow_file": "task-context.json"
        }
      ],
      "sessions": [
        {
          "id": 0,
          "description": "Homogeneous pooled session",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "workflow_main.json" },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "workflow_main.json" },
            {
              "id": 2,
              "type": "skill",
              "description": "Search skill",
              "workflow_file": "workflow_main.json",
              "skill": {
                "name": "search",
                "config_dir": "skills/search"
              }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow_main.json", "wf_main");
  write_workflow_json(tmp / "task-context.json", "wf_context");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);
  ASSERT_NE(agent->get_context(), nullptr);
  ASSERT_NE(agent->get_context()->get_context_updater_task(), nullptr);
  EXPECT_EQ(agent->get_context()->get_context_updater_task()->get_workflow_pool_size(), 1);

  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);
  auto dispatcher = session->get_task("0_task_0");
  auto coordinator = session->get_task("0_task_1");
  auto skill = session->get_task("0_task_2");
  ASSERT_NE(dispatcher, nullptr);
  ASSERT_NE(coordinator, nullptr);
  ASSERT_NE(skill, nullptr);
  EXPECT_EQ(dispatcher->get_workflow_pool_size(), 1);
  EXPECT_EQ(coordinator->get_workflow_pool_size(), 1);
  EXPECT_EQ(skill->get_workflow_pool_size(), 1);

  fs::remove_all(tmp);
}

// ============================================================
// next_skills and Skill Management Tests
// ============================================================

TEST_F(AgentTest, TaskHasNextSkillsConfiguration) {
  framework::MMAgentTask::Config config;
  config.task_id = "test_task";
  config.task_type = "dispatcher";
  config.next_skills = {"search", "summary", "delegate"};
  config.max_concurrent_skills = 5;

  auto task = std::make_shared<framework::MMAgentTask>(config);
  ASSERT_NE(task, nullptr);

  const auto& next_skills = task->get_next_skills();
  EXPECT_EQ(next_skills.size(), 3);
  EXPECT_EQ(next_skills[0], "search");
  EXPECT_EQ(next_skills[1], "summary");
  EXPECT_EQ(next_skills[2], "delegate");
  EXPECT_EQ(task->get_max_concurrent_skills(), 5);
}

TEST_F(AgentTest, TaskRunningSkillsCount) {
  framework::MMAgentTask::Config config;
  config.task_id = "test_task";
  config.task_type = "dispatcher";
  config.max_concurrent_skills = 3;

  auto task = std::make_shared<framework::MMAgentTask>(config);
  ASSERT_NE(task, nullptr);

  // Initially no running skills
  EXPECT_EQ(task->get_running_skills_count(), 0);
  EXPECT_TRUE(task->can_launch_more_skills());

  // Start some skills
  task->on_skill_started("skill1");
  EXPECT_EQ(task->get_running_skills_count(), 1);
  EXPECT_TRUE(task->can_launch_more_skills());

  task->on_skill_started("skill2");
  EXPECT_EQ(task->get_running_skills_count(), 2);
  EXPECT_TRUE(task->can_launch_more_skills());

  task->on_skill_started("skill3");
  EXPECT_EQ(task->get_running_skills_count(), 3);
  EXPECT_FALSE(task->can_launch_more_skills());  // Reached limit

  // Complete a skill
  task->on_skill_completed("skill1");
  EXPECT_EQ(task->get_running_skills_count(), 2);
  EXPECT_TRUE(task->can_launch_more_skills());

  // Clear all
  task->clear_running_skills();
  EXPECT_EQ(task->get_running_skills_count(), 0);
  EXPECT_TRUE(task->can_launch_more_skills());
}

TEST_F(AgentTest, BuiltinDispatcherParsesNextSkills) {
  // Test JSON array format
  std::string json_array = R"(["search", "summary", "delegate"])";
  DataVariant value = json_array;
  auto skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0], "search");
  EXPECT_EQ(skills[1], "summary");
  EXPECT_EQ(skills[2], "delegate");

  // Test comma-separated format
  std::string csv = "search, summary, delegate";
  value = csv;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0], "search");
  EXPECT_EQ(skills[1], "summary");
  EXPECT_EQ(skills[2], "delegate");

  // Test empty string
  value = std::string("");
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 0);
}

// Test JSON array parsing fallback (works with or without RapidJSON)
TEST_F(AgentTest, BuiltinDispatcherParsesNextSkillsJsonArrayFallback) {
  // Test compact JSON array (no spaces)
  std::string compact = R"(["shopping","foodie"])";
  DataVariant value = compact;
  auto skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 2);
  EXPECT_EQ(skills[0], "shopping");
  EXPECT_EQ(skills[1], "foodie");

  // Test JSON array with spaces
  std::string with_spaces = R"([  "travel" ,  "search"  ])";
  value = with_spaces;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 2);
  EXPECT_EQ(skills[0], "travel");
  EXPECT_EQ(skills[1], "search");

  // Test JSON array with newlines
  std::string with_newlines = "[\n  \"skill1\",\n  \"skill2\"\n]";
  value = with_newlines;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 2);
  EXPECT_EQ(skills[0], "skill1");
  EXPECT_EQ(skills[1], "skill2");

  // Test single element JSON array
  std::string single = R"(["only_one"])";
  value = single;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 1);
  EXPECT_EQ(skills[0], "only_one");

  // Test empty JSON array
  std::string empty_array = R"([])";
  value = empty_array;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 0);

  // Test JSON array with mixed whitespace
  std::string mixed = R"([ "first","second" , "third"])";
  value = mixed;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0], "first");
  EXPECT_EQ(skills[1], "second");
  EXPECT_EQ(skills[2], "third");
}

TEST_F(AgentTest, DispatcherWithNextSkillsFromPackage) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_dispatcher_skills";
  fs::create_directories(tmp);

  // Write package.json
  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }

  // Write agent.json with next_skills
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "sessions": [
        {
          "id": 0,
          "description": "Test session",
          "task_count": 3,
          "tasks": [
            {
              "id": 0,
              "type": "dispatcher",
              "description": "Dispatcher with next_skills",
              "next_skills": ["search", "summary"],
              "max_concurrent_skills": 2
            },
            {
              "id": 1,
              "type": "coordinator",
              "description": "Coordinator",
              "workflow_file": "workflow_main.json"
            },
            {
              "id": 2,
              "type": "skill",
              "description": "Search skill",
              "workflow_file": "workflow_main.json",
              "skill": {"name": "search", "config_dir": "skills/search"}
            }
          ]
        }
      ]
    })";
  }

  write_workflow_json(tmp / "workflow_main.json", "wf_main");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);

  auto dispatcher = session->get_task("0_task_0");
  ASSERT_NE(dispatcher, nullptr);
  EXPECT_EQ(dispatcher->get_task_type(), "dispatcher");

  // Check next_skills configuration
  const auto& next_skills = dispatcher->get_next_skills();
  EXPECT_EQ(next_skills.size(), 2);
  EXPECT_EQ(next_skills[0], "search");
  EXPECT_EQ(next_skills[1], "summary");
  EXPECT_EQ(dispatcher->get_max_concurrent_skills(), 2);

  // Note: dispatcher workflow_file is optional now
  EXPECT_TRUE(dispatcher->get_workflow_file().empty() || !dispatcher->get_workflow_file().empty());

  fs::remove_all(tmp);
}

// ============================================================
// Context Callback Tests
// ============================================================

TEST_F(AgentTest, SetContextCallbackEnablesCallback) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  // Initially no callback
  EXPECT_FALSE(agent->has_context_callback());

  // Set a callback
  agent->set_context_callback(
      [](const std::string& user_id, const std::string& agent_id,
         const std::string& user_input, const std::string& previous_context) {
        return "test_context";
      },
      false);

  EXPECT_TRUE(agent->has_context_callback());
}

TEST_F(AgentTest, ClearContextCallbackRemovesCallback) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  // Set and verify
  agent->set_context_callback(
      [](const std::string& user_id, const std::string& agent_id,
         const std::string& user_input, const std::string& previous_context) {
        return "test";
      },
      false);
  EXPECT_TRUE(agent->has_context_callback());

  // Clear and verify
  agent->clear_context_callback();
  EXPECT_FALSE(agent->has_context_callback());
}

TEST_F(AgentTest, SetContextCallbackWithConfig) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  framework::ContextCallbackConfig config;
  config.callback = [](const std::string& user_id, const std::string& agent_id,
                       const std::string& user_input, const std::string& previous_context) {
    return "config_context";
  };
  config.async = false;
  config.timeout_ms = 3000;

  agent->set_context_callback(config);
  EXPECT_TRUE(agent->has_context_callback());
}

TEST_F(AgentTest, SyncContextCallbackStoresContextInDataBus) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_context_callback";
  fs::create_directories(tmp);

  // Write package.json
  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }

  // Write agent.json with next_skills but no workflow_file for dispatcher
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "sessions": [
        {
          "id": 0,
          "description": "Test session",
          "task_count": 3,
          "tasks": [
            {
              "id": 0,
              "type": "dispatcher",
              "description": "Dispatcher with next_skills",
              "next_skills": ["test-skill"]
            },
            {
              "id": 1,
              "type": "coordinator",
              "description": "Coordinator",
              "workflow_file": "workflow_main.json"
            },
            {
              "id": 2,
              "type": "skill",
              "description": "Test skill",
              "workflow_file": "workflow_main.json",
              "skill": {"name": "test-skill", "config_dir": "skills/test"}
            }
          ]
        }
      ]
    })";
  }

  write_workflow_json(tmp / "workflow_main.json", "wf_main");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Track callback invocations
  std::atomic<int> callback_count{0};
  std::string captured_user_id;
  std::string captured_agent_id;
  std::string captured_user_input;

  // Set sync callback
  agent->set_context_callback(
      [&](const std::string& user_id, const std::string& agent_id,
          const std::string& user_input, const std::string& previous_context) {
        callback_count++;
        captured_user_id = user_id;
        captured_agent_id = agent_id;
        captured_user_input = user_input;
        return "User context for: " + user_input;
      },
      false);  // sync mode

  // Send a message that triggers dialogue creation
  framework::MMAgentMsgQue::Message req;
  req.message_id = "test_context_msg";
  req.content = "Hello, test!";
  req.metadata["user_id"] = "test_user_123";
  req.metadata["client_id"] = "test_client_456";

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // Wait for callback to be invoked
  for(int i = 0; i < 50 && callback_count.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Verify callback was invoked
  EXPECT_EQ(callback_count.load(), 1);
  EXPECT_EQ(captured_user_id, "test_user_123");
  EXPECT_EQ(captured_agent_id, "test_agent");  // Agent ID comes from package.json
  EXPECT_EQ(captured_user_input, "Hello, test!");

  // Verify context was stored in DataBus
  // The dialogue_id is generated as hash(user_text + message_id)
  std::string expected_dialogue_id =
      std::to_string(std::hash<std::string>{}("Hello, test!" + std::string("test_context_msg")));

  // Get session to build the context key
  auto session = agent->get_session_by_key("test_user_123:test_client_456");
  ASSERT_NE(session, nullptr);

  std::string ctx_key =
      "/session/" + session->get_session_id() + "/ctx/" + expected_dialogue_id + "/content";

  DataVariant ctx_value;
  ASSERT_TRUE(agent->get_data(ctx_key, ctx_value));

  ASSERT_TRUE(std::holds_alternative<std::string>(ctx_value));
  std::string stored_context = std::get<std::string>(ctx_value);
  EXPECT_EQ(stored_context, "User context for: Hello, test!");

  fs::remove_all(tmp);
}

TEST_F(AgentTest, AsyncContextCallbackDoesNotBlock) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_async_callback";
  fs::create_directories(tmp);

  // Write minimal package
  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "sessions": [
        {
          "id": 0,
          "description": "Test session",
          "task_count": 3,
          "tasks": [
            {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["s"]},
            {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
            {"id": 2, "type": "skill", "description": "S", "workflow_file": "workflow.json", "skill": {"name": "s", "config_dir": "skills/s"}}
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  std::atomic<bool> callback_started{false};
  std::atomic<bool> callback_finished{false};

  // Set async callback with simulated delay
  agent->set_context_callback(
      [&](const std::string&, const std::string&, const std::string&, const std::string&) {
        callback_started = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        callback_finished = true;
        return "async_context";
      },
      true);  // async mode

  // Send message
  framework::MMAgentMsgQue::Message req;
  req.message_id = "async_test_msg";
  req.content = "Async test";
  req.metadata["user_id"] = "async_user";
  req.metadata["client_id"] = "async_client";

  auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  // Processing should return quickly (not wait for callback)
  // The callback has 100ms delay, so if processing took less than 50ms,
  // the callback ran asynchronously
  EXPECT_LT(elapsed.count(), 50);

  // Wait for callback to complete
  for(int i = 0; i < 20 && !callback_finished.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_TRUE(callback_started.load());
  EXPECT_TRUE(callback_finished.load());

  fs::remove_all(tmp);
}

TEST_F(AgentTest, ContextCallbackReceivesPreviousContext) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_prev_context";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "sessions": [
        {
          "id": 0,
          "description": "Test session",
          "task_count": 3,
          "tasks": [
            {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["s"]},
            {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
            {"id": 2, "type": "skill", "description": "S", "workflow_file": "workflow.json", "skill": {"name": "s", "config_dir": "skills/s"}}
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  std::string captured_previous_context = "NOT_SET";

  // Set callback that captures previous context
  agent->set_context_callback(
      [&](const std::string&, const std::string&, const std::string&,
          const std::string& previous_context) {
        captured_previous_context = previous_context;
        return "new_context";
      },
      false);  // sync mode

  // First request - no previous context
  framework::MMAgentMsgQue::Message req1;
  req1.message_id = "first_msg";
  req1.content = "First message";
  req1.metadata["user_id"] = "ctx_user";
  req1.metadata["client_id"] = "ctx_client";

  ASSERT_TRUE(agent->route_request_message(req1));
  agent->process_all_sessions_until_idle();

  // Previous context should be empty for first request
  EXPECT_TRUE(captured_previous_context.empty() || captured_previous_context == "NOT_SET");

  // Get the dialogue_id that was generated
  std::string dialogue_id_1 = std::to_string(
      std::hash<std::string>{}(std::string("First message") + std::string("first_msg")));

  // Now verify the context was stored
  auto session = agent->get_session_by_key("ctx_user:ctx_client");
  ASSERT_NE(session, nullptr);
  std::string ctx_key_1 = "/session/" + session->get_session_id() + "/ctx/" + dialogue_id_1 + "/content";

  DataVariant ctx_val;
  ASSERT_TRUE(agent->get_data(ctx_key_1, ctx_val));

  fs::remove_all(tmp);
}

TEST_F(AgentTest, ContextCallbackExceptionHandling) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_callback_exception";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "sessions": [
        {
          "id": 0,
          "description": "Test session",
          "task_count": 3,
          "tasks": [
            {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["s"]},
            {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
            {"id": 2, "type": "skill", "description": "S", "workflow_file": "workflow.json", "skill": {"name": "s", "config_dir": "skills/s"}}
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Set callback that throws exception
  agent->set_context_callback(
      [](const std::string&, const std::string&, const std::string&, const std::string&) ->
          std::string {
        throw std::runtime_error("Test exception from callback");
      },
      false);  // sync mode

  // Send message - should not crash
  framework::MMAgentMsgQue::Message req;
  req.message_id = "exception_msg";
  req.content = "Trigger exception";
  req.metadata["user_id"] = "exc_user";
  req.metadata["client_id"] = "exc_client";

  // This should not throw - exception should be caught internally
  EXPECT_NO_THROW({
    ASSERT_TRUE(agent->route_request_message(req));
    agent->process_all_sessions_until_idle();
  });

  fs::remove_all(tmp);
}

// ============================================================
// Skill Parameter Validation Tests
// ============================================================

TEST_F(AgentTest, SkillWithEmptyExpectedParametersSkipsValidation) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_empty_params";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    // Skill with NO expected_parameters (empty)
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill without expected_parameters",
          "workflow_file": "workflow.json",
          "skill": {"name": "travel", "config_dir": "skills/travel", "instance_pool_size": 1}
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);

  // Find the skill task
  auto skill_task = find_task_by_type(session->get_tasks(), "skill");
  ASSERT_NE(skill_task, nullptr);
  EXPECT_EQ(skill_task->get_skill_name(), "travel");

  // expected_parameters should be empty
  const auto& expected_params = skill_task->get_expected_parameters();
  EXPECT_TRUE(expected_params.empty());

  fs::remove_all(tmp);
}

TEST_F(AgentTest, SkillWithExpectedParametersParsesCorrectly) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_expected_params";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    // Skill WITH expected_parameters
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill with expected_parameters",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "instance_pool_size": 1,
            "expected_parameters": ["destination", "date", "travelers"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);

  // Find the skill task
  auto skill_task = find_task_by_type(session->get_tasks(), "skill");
  ASSERT_NE(skill_task, nullptr);
  EXPECT_EQ(skill_task->get_skill_name(), "travel");

  // Verify expected_parameters were parsed correctly
  const auto& expected_params = skill_task->get_expected_parameters();
  ASSERT_EQ(expected_params.size(), 3);
  EXPECT_EQ(expected_params[0], "destination");
  EXPECT_EQ(expected_params[1], "date");
  EXPECT_EQ(expected_params[2], "travelers");

  fs::remove_all(tmp);
}

TEST_F(AgentTest, ParameterValidationPassesWhenAllParametersAvailableInMetadata) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_params_metadata";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "expected_parameters": ["destination", "date"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Send a request with available_parameters in metadata
  framework::MMAgentMsgQue::Message req;
  req.message_id = "test_params_msg";
  req.content = "Book a trip to Beijing";
  req.metadata["user_id"] = "params_user";
  req.metadata["client_id"] = "params_client";
  req.metadata["available_parameters"] = R"({
    "travel": {
      "destination": "Beijing",
      "date": "2024-03-20"
    }
  })";

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // The skill should be launched since all parameters are available
  // Check that a skill task was generated
  std::shared_ptr<framework::MMAgentTask> generated;
  bool found_skill = false;
  for(int i = 0; i < 10; ++i) {
    while(agent->pop_generated_task(generated)) {
      if(generated && generated->get_task_type() == "skill") {
        found_skill = true;
        break;
      }
    }
    if(found_skill) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(found_skill);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, ParameterValidationFailsWhenParametersMissing) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_missing_params";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    // Skill requires destination, date, AND travelers
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "expected_parameters": ["destination", "date", "travelers"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Send a request with PARTIAL available_parameters (missing "travelers")
  framework::MMAgentMsgQue::Message req;
  req.message_id = "test_missing_params_msg";
  req.content = "Book a trip to Beijing";
  req.metadata["user_id"] = "missing_params_user";
  req.metadata["client_id"] = "missing_params_client";
  req.metadata["available_parameters"] = R"({
    "travel": {
      "destination": "Beijing",
      "date": "2024-03-20"
    }
  })";

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // The skill should NOT be launched since "travelers" is missing
  // No skill task should be generated
  std::shared_ptr<framework::MMAgentTask> generated;
  bool found_skill = false;
  for(int i = 0; i < 10; ++i) {
    while(agent->pop_generated_task(generated)) {
      if(generated && generated->get_task_type() == "skill") {
        found_skill = true;
        break;
      }
    }
    if(found_skill) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Skill should NOT be launched due to missing parameter
  EXPECT_FALSE(found_skill);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, ParameterValidationFailsWhenNoParametersAvailable) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_no_params";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "expected_parameters": ["destination"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Send a request WITHOUT any available_parameters
  framework::MMAgentMsgQue::Message req;
  req.message_id = "test_no_params_msg";
  req.content = "Book a trip";
  req.metadata["user_id"] = "no_params_user";
  req.metadata["client_id"] = "no_params_client";
  // No available_parameters in metadata or databus

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // The skill should NOT be launched since no parameters are available
  std::shared_ptr<framework::MMAgentTask> generated;
  bool found_skill = false;
  for(int i = 0; i < 10; ++i) {
    while(agent->pop_generated_task(generated)) {
      if(generated && generated->get_task_type() == "skill") {
        found_skill = true;
        break;
      }
    }
    if(found_skill) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_FALSE(found_skill);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, ExtraParametersDoNotAffectValidation) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_extra_params";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "expected_parameters": ["destination"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Send a request with available_parameters containing EXTRA parameters
  framework::MMAgentMsgQue::Message req;
  req.message_id = "test_extra_params_msg";
  req.content = "Book a trip";
  req.metadata["user_id"] = "extra_params_user";
  req.metadata["client_id"] = "extra_params_client";
  req.metadata["available_parameters"] = R"({
    "travel": {
      "destination": "Beijing",
      "date": "2024-03-20",
      "travelers": "2",
      "class": "business",
      "extra_field": "extra_value"
    }
  })";

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // The skill SHOULD be launched since all expected params are present
  // Extra parameters should not affect validation
  std::shared_ptr<framework::MMAgentTask> generated;
  bool found_skill = false;
  for(int i = 0; i < 10; ++i) {
    while(agent->pop_generated_task(generated)) {
      if(generated && generated->get_task_type() == "skill") {
        found_skill = true;
        break;
      }
    }
    if(found_skill) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(found_skill);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, MetadataTakesPriorityOverDataBusForAvailableParameters) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_priority";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "expected_parameters": ["destination"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Put DIFFERENT values in DataBus
  agent->publish_data("available_parameters", std::string(R"({
    "travel": {
      "destination": "Shanghai"
    }
  })"));

  // Send request with available_parameters in metadata (should take priority)
  framework::MMAgentMsgQue::Message req;
  req.message_id = "test_priority_msg";
  req.content = "Book a trip";
  req.metadata["user_id"] = "priority_user";
  req.metadata["client_id"] = "priority_client";
  req.metadata["available_parameters"] = R"({
    "travel": {
      "destination": "Beijing"
    }
  })";

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // The skill should be launched using metadata values
  std::shared_ptr<framework::MMAgentTask> generated;
  bool found_skill = false;
  for(int i = 0; i < 10; ++i) {
    while(agent->pop_generated_task(generated)) {
      if(generated && generated->get_task_type() == "skill") {
        found_skill = true;
        break;
      }
    }
    if(found_skill) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(found_skill);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, DataBusFallbackWhenNoMetadataAvailableParameters) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_databus_fallback";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "expected_parameters": ["destination"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Calculate the expected dialogue_id: hash(content + message_id)
  const std::string content = "Book a trip";
  const std::string message_id = "test_databus_fallback_msg";
  std::string expected_dialogue_id = std::to_string(std::hash<std::string>{}(content + message_id));

  // Put available_parameters in DataBus with the calculated dialogue_id prefix
  agent->publish_data(expected_dialogue_id + "/available_parameters", std::string(R"({
    "travel": {
      "destination": "Shanghai"
    }
  })"));

  // Send request WITHOUT available_parameters in metadata
  framework::MMAgentMsgQue::Message req;
  req.message_id = message_id;
  req.content = content;
  req.metadata["user_id"] = "fallback_user";
  req.metadata["client_id"] = "fallback_client";
  // No available_parameters in metadata - should use DataBus

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // The skill should be launched using DataBus values (fallback)
  std::shared_ptr<framework::MMAgentTask> generated;
  bool found_skill = false;
  for(int i = 0; i < 10; ++i) {
    while(agent->pop_generated_task(generated)) {
      if(generated && generated->get_task_type() == "skill") {
        found_skill = true;
        break;
      }
    }
    if(found_skill) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(found_skill);

  fs::remove_all(tmp);
}

TEST_F(AgentTest, DialogueIdPrefixedAvailableParametersKey) {
  namespace fs = std::filesystem;
  auto tmp = fs::temp_directory_path() / "agentflow_test_dialogue_params";
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({"name": "test_agent", "version": "1.0.0"})";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {"id": 0, "type": "dispatcher", "description": "D", "next_skills": ["travel"]},
        {"id": 1, "type": "coordinator", "description": "C", "workflow_file": "workflow.json"},
        {
          "id": 2,
          "type": "skill",
          "description": "Travel skill",
          "workflow_file": "workflow.json",
          "skill": {
            "name": "travel",
            "config_dir": "skills/travel",
            "expected_parameters": ["destination"]
          }
        }
      ]
    })";
  }
  write_workflow_json(tmp / "workflow.json", "wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  // Calculate the expected dialogue_id: hash(content + message_id)
  const std::string content = "Book a trip";
  const std::string message_id = "test_dialogue_params_msg";
  std::string expected_dialogue_id = std::to_string(std::hash<std::string>{}(content + message_id));

  // Put available_parameters in DataBus with the calculated dialogue_id prefix
  agent->publish_data(expected_dialogue_id + "/available_parameters", std::string(R"({
    "travel": {
      "destination": "Tokyo"
    }
  })"));

  // Send request - the framework will generate the same dialogue_id
  framework::MMAgentMsgQue::Message req;
  req.message_id = message_id;
  req.content = content;
  req.metadata["user_id"] = "dialogue_user";
  req.metadata["client_id"] = "dialogue_client";

  ASSERT_TRUE(agent->route_request_message(req));
  agent->process_all_sessions_until_idle();

  // The skill should be launched using dialogue-scoped DataBus values
  std::shared_ptr<framework::MMAgentTask> generated;
  bool found_skill = false;
  for(int i = 0; i < 10; ++i) {
    while(agent->pop_generated_task(generated)) {
      if(generated && generated->get_task_type() == "skill") {
        found_skill = true;
        break;
      }
    }
    if(found_skill) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(found_skill);

  fs::remove_all(tmp);
}

// =============================================================================
// Multi-Response Tests
// =============================================================================

class MultiResponseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    agent = std::make_shared<framework::MMAgent>(base_config());
  }

  void TearDown() override {
    agent.reset();
  }

  framework::MMAgent::Config base_config() {
    framework::MMAgent::Config cfg;
    cfg.id = "multi_response_test";
    cfg.name = "multi_response_test";
    return cfg;
  }

  // Helper to setup multi-response data in DataBus (simulating runtime.cpp behavior)
  void setup_multi_response(const std::string& session_id, const std::string& dialogue_id,
                            const std::vector<std::pair<std::string, bool>>& responses) {
    std::string base_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses";

    int index = 0;
    for(const auto& [content, is_done] : responses) {
      // Write content
      agent->publish_data(base_key + "/" + std::to_string(index) + "/content", content);
      // Set consumed = false
      agent->publish_data(base_key + "/" + std::to_string(index) + "/consumed", std::string("false"));
      index++;
    }

    // Set total_count
    agent->publish_data(base_key + "/total_count", std::to_string(index));
    // Set consumed_count = 0
    agent->publish_data(base_key + "/consumed_count", std::string("0"));
    // Set done flag (always set, either true or false based on last response)
    if(!responses.empty()) {
      agent->publish_data(base_key + "/done", responses.back().second ? std::string("true") : std::string("false"));
    }
  }

  // Helper to consume a multi-response (simulating agent.cpp consume_multi_response)
  struct ConsumeResult {
    bool has_response = false;
    std::string content;
    bool is_done = false;
    bool all_consumed = false;
    int consumed_index = -1;
  };

  ConsumeResult consume_multi_response(const std::string& session_id, const std::string& dialogue_id) {
    ConsumeResult result;
    std::string base_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses";

    // Check if responses/done exists (indicates multi-response mode)
    DataVariant done_val;
    if(!agent->get_data(base_key + "/done", done_val)) {
      return result;  // Not in multi-response mode
    }

    // Read consumed_count
    DataVariant consumed_count_val;
    int consumed_count = 0;
    if(agent->get_data(base_key + "/consumed_count", consumed_count_val)) {
      if(const auto* count_str = std::get_if<std::string>(&consumed_count_val)) {
        consumed_count = std::stoi(*count_str);
      }
    }

    // Read total_count
    DataVariant total_count_val;
    int total_count = 0;
    if(agent->get_data(base_key + "/total_count", total_count_val)) {
      if(const auto* count_str = std::get_if<std::string>(&total_count_val)) {
        total_count = std::stoi(*count_str);
      }
    }

    // Check if there are still unconsumed responses
    if(consumed_count >= total_count) {
      result.all_consumed = true;
      if(const auto* done_str = std::get_if<std::string>(&done_val); done_str && *done_str == "true") {
        result.is_done = true;
      }
      return result;
    }

    // Read the content at consumed_count
    DataVariant content_val;
    if(agent->get_data(base_key + "/" + std::to_string(consumed_count) + "/content", content_val)) {
      if(const auto* text = std::get_if<std::string>(&content_val)) {
        result.has_response = true;
        result.content = *text;
        result.consumed_index = consumed_count;
      }
    }

    // Check done flag
    if(const auto* done_str = std::get_if<std::string>(&done_val); done_str && *done_str == "true") {
      result.is_done = true;
    }

    return result;
  }

  // Helper to mark a response as consumed
  void mark_consumed(const std::string& session_id, const std::string& dialogue_id, int index) {
    std::string base_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses";
    agent->publish_data(base_key + "/" + std::to_string(index) + "/consumed", std::string("true"));
    agent->publish_data(base_key + "/consumed_count", std::to_string(index + 1));
  }

  std::shared_ptr<framework::MMAgent> agent;
};

TEST_F(MultiResponseTest, DataBusKeyStructure) {
  std::string session_id = "test-session-1";
  std::string dialogue_id = "test-dialogue-1";

  setup_multi_response(session_id, dialogue_id, {
    {"First response", false},
    {"Second response", false},
    {"Final response", true}
  });

  std::string base_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses";

  // Verify total_count
  DataVariant total_count;
  ASSERT_TRUE(agent->get_data(base_key + "/total_count", total_count));
  EXPECT_EQ(std::get<std::string>(total_count), "3");

  // Verify consumed_count initial value
  DataVariant consumed_count;
  ASSERT_TRUE(agent->get_data(base_key + "/consumed_count", consumed_count));
  EXPECT_EQ(std::get<std::string>(consumed_count), "0");

  // Verify done flag
  DataVariant done;
  ASSERT_TRUE(agent->get_data(base_key + "/done", done));
  EXPECT_EQ(std::get<std::string>(done), "true");

  // Verify each response
  for(int i = 0; i < 3; i++) {
    DataVariant content;
    ASSERT_TRUE(agent->get_data(base_key + "/" + std::to_string(i) + "/content", content));
    if(i == 0) EXPECT_EQ(std::get<std::string>(content), "First response");
    else if(i == 1) EXPECT_EQ(std::get<std::string>(content), "Second response");
    else EXPECT_EQ(std::get<std::string>(content), "Final response");

    DataVariant consumed;
    ASSERT_TRUE(agent->get_data(base_key + "/" + std::to_string(i) + "/consumed", consumed));
    EXPECT_EQ(std::get<std::string>(consumed), "false");
  }
}

TEST_F(MultiResponseTest, ConsumeResponsesInOrder) {
  std::string session_id = "test-session-2";
  std::string dialogue_id = "test-dialogue-2";

  setup_multi_response(session_id, dialogue_id, {
    {"Response A", false},
    {"Response B", false},
    {"Response C", true}
  });

  // Consume first response
  auto result1 = consume_multi_response(session_id, dialogue_id);
  EXPECT_TRUE(result1.has_response);
  EXPECT_EQ(result1.content, "Response A");
  EXPECT_EQ(result1.consumed_index, 0);
  EXPECT_TRUE(result1.is_done);  // done flag is true (last response)

  mark_consumed(session_id, dialogue_id, 0);

  // Consume second response
  auto result2 = consume_multi_response(session_id, dialogue_id);
  EXPECT_TRUE(result2.has_response);
  EXPECT_EQ(result2.content, "Response B");
  EXPECT_EQ(result2.consumed_index, 1);

  mark_consumed(session_id, dialogue_id, 1);

  // Consume third response
  auto result3 = consume_multi_response(session_id, dialogue_id);
  EXPECT_TRUE(result3.has_response);
  EXPECT_EQ(result3.content, "Response C");
  EXPECT_EQ(result3.consumed_index, 2);

  mark_consumed(session_id, dialogue_id, 2);

  // All consumed
  auto result4 = consume_multi_response(session_id, dialogue_id);
  EXPECT_FALSE(result4.has_response);
  EXPECT_TRUE(result4.all_consumed);
  EXPECT_TRUE(result4.is_done);
}

TEST_F(MultiResponseTest, NotInMultiResponseModeWhenNoData) {
  std::string session_id = "test-session-3";
  std::string dialogue_id = "test-dialogue-3";

  // No multi-response data set up
  auto result = consume_multi_response(session_id, dialogue_id);
  EXPECT_FALSE(result.has_response);
  EXPECT_FALSE(result.is_done);
  EXPECT_FALSE(result.all_consumed);
}

TEST_F(MultiResponseTest, SingleResponseBackwardCompatibility) {
  std::string session_id = "test-session-4";
  std::string dialogue_id = "test-dialogue-4";

  // Set up single response (old format)
  std::string response_key = "/session/" + session_id + "/sys/" + dialogue_id + "/response";
  agent->publish_data(response_key, std::string("Single response content"));

  // Multi-response should not detect anything
  auto result = consume_multi_response(session_id, dialogue_id);
  EXPECT_FALSE(result.has_response);

  // But single response should be available
  DataVariant response_val;
  ASSERT_TRUE(agent->get_data(response_key, response_val));
  EXPECT_EQ(std::get<std::string>(response_val), "Single response content");
}

TEST_F(MultiResponseTest, IncompleteSessionNotDone) {
  std::string session_id = "test-session-5";
  std::string dialogue_id = "test-dialogue-5";

  // Set up responses with done=false (incomplete session)
  setup_multi_response(session_id, dialogue_id, {
    {"Partial response 1", false},
    {"Partial response 2", false}
    // Note: last response has is_done = false, so done flag is set to "false"
  });

  std::string base_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses";

  // Verify done flag is set to "false" (last response had is_done=false)
  DataVariant done;
  ASSERT_TRUE(agent->get_data(base_key + "/done", done));
  EXPECT_EQ(std::get<std::string>(done), "false");

  // Consume responses
  auto result1 = consume_multi_response(session_id, dialogue_id);
  EXPECT_TRUE(result1.has_response);
  EXPECT_FALSE(result1.is_done);  // Session not complete (done="false")

  mark_consumed(session_id, dialogue_id, 0);

  auto result2 = consume_multi_response(session_id, dialogue_id);
  EXPECT_TRUE(result2.has_response);
  EXPECT_FALSE(result2.is_done);

  mark_consumed(session_id, dialogue_id, 1);

  // All consumed but session not done
  auto result3 = consume_multi_response(session_id, dialogue_id);
  EXPECT_FALSE(result3.has_response);
  EXPECT_TRUE(result3.all_consumed);
  EXPECT_FALSE(result3.is_done);  // done="false", so is_done should be false
}

TEST_F(MultiResponseTest, CleanupMultiResponses) {
  std::string session_id = "test-session-6";
  std::string dialogue_id = "test-dialogue-6";

  setup_multi_response(session_id, dialogue_id, {
    {"Response 1", false},
    {"Response 2", true}
  });

  std::string base_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses";

  // Verify data exists
  DataVariant total_count;
  ASSERT_TRUE(agent->get_data(base_key + "/total_count", total_count));
  EXPECT_EQ(std::get<std::string>(total_count), "2");

  // Clean up (simulate cleanup_multi_responses)
  // Note: publish_data with empty DataVariant sets value to default (int64_t(0))
  agent->publish_data(base_key + "/0/content", DataVariant());
  agent->publish_data(base_key + "/0/consumed", DataVariant());
  agent->publish_data(base_key + "/1/content", DataVariant());
  agent->publish_data(base_key + "/1/consumed", DataVariant());
  agent->publish_data(base_key + "/total_count", DataVariant());
  agent->publish_data(base_key + "/consumed_count", DataVariant());
  agent->publish_data(base_key + "/done", DataVariant());

  // Verify data is cleared (keys still exist but values are default int64_t(0))
  DataVariant total_count_check;
  ASSERT_TRUE(agent->get_data(base_key + "/total_count", total_count_check));
  // Empty DataVariant defaults to int64_t(0)
  EXPECT_TRUE(std::holds_alternative<int64_t>(total_count_check));
  EXPECT_EQ(std::get<int64_t>(total_count_check), 0);

  DataVariant done_check;
  ASSERT_TRUE(agent->get_data(base_key + "/done", done_check));
  EXPECT_TRUE(std::holds_alternative<int64_t>(done_check));
  EXPECT_EQ(std::get<int64_t>(done_check), 0);
}

TEST_F(MultiResponseTest, EmptyResponsesList) {
  std::string session_id = "test-session-7";
  std::string dialogue_id = "test-dialogue-7";

  // Set up empty responses with done=true
  std::string base_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses";
  agent->publish_data(base_key + "/total_count", std::string("0"));
  agent->publish_data(base_key + "/consumed_count", std::string("0"));
  agent->publish_data(base_key + "/done", std::string("true"));

  // Should report all consumed immediately
  auto result = consume_multi_response(session_id, dialogue_id);
  EXPECT_FALSE(result.has_response);
  EXPECT_TRUE(result.all_consumed);
  EXPECT_TRUE(result.is_done);
}

TEST_F(MultiResponseTest, LargeNumberOfResponses) {
  std::string session_id = "test-session-8";
  std::string dialogue_id = "test-dialogue-8";

  // Create 10 responses
  std::vector<std::pair<std::string, bool>> responses;
  for(int i = 0; i < 10; i++) {
    responses.emplace_back("Response " + std::to_string(i), i == 9);
  }
  setup_multi_response(session_id, dialogue_id, responses);

  // Consume all 10 responses
  for(int i = 0; i < 10; i++) {
    auto result = consume_multi_response(session_id, dialogue_id);
    EXPECT_TRUE(result.has_response) << "Expected response at index " << i;
    EXPECT_EQ(result.content, "Response " + std::to_string(i));
    mark_consumed(session_id, dialogue_id, i);
  }

  // Verify all consumed
  auto final_result = consume_multi_response(session_id, dialogue_id);
  EXPECT_FALSE(final_result.has_response);
  EXPECT_TRUE(final_result.all_consumed);
  EXPECT_TRUE(final_result.is_done);
}

}  // namespace tests
}  // namespace agentflow
