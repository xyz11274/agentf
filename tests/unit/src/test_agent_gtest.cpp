#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "agent/agent.h"
#include "agent/context.h"
#include "agent/msgque.h"
#include "agent/session.h"
#include "agent/task.h"
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
  EXPECT_TRUE(out.content.empty());
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
  auto pooled_session = agent->get_session_by_key("0");
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
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 3u);
  auto collaboration_task = find_task_by_type(tasks, "collaboration");
  ASSERT_NE(collaboration_task, nullptr);
  EXPECT_EQ(collaboration_task->get_session_id(), "0");
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
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 3u);
  auto dispatcher_task = session->get_task("0_task_0");
  auto coordinator_task = session->get_task("0_task_1");
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

  auto session = agent->get_session_by_key("0");
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

  auto session = agent->get_session_by_key("0");
  ASSERT_NE(session, nullptr);
  auto task = find_task_by_message_id(session->get_tasks(), "req_workflow_override");
  ASSERT_NE(task, nullptr);
  EXPECT_EQ(task->get_workflow_id(), "wf_a");

  fs::remove_all(tmp);
}

TEST_F(AgentTest, DefaultPooledSessionContainsDispatcherAndCoordinator) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  auto pooled_session = agent->get_session_by_key("0");
  ASSERT_NE(pooled_session, nullptr);
  ASSERT_NE(pooled_session->get_task_coordinator(), nullptr);
  ASSERT_NE(pooled_session->get_task_dispatcher(), nullptr);
  auto default_dispatcher = pooled_session->get_task("0_task_0");
  auto default_coordinator = pooled_session->get_task("0_task_1");
  ASSERT_NE(default_dispatcher, nullptr);
  ASSERT_NE(default_coordinator, nullptr);
  EXPECT_EQ(default_dispatcher->get_task_type(), "dispatcher");
  EXPECT_EQ(default_coordinator->get_task_type(), "coordinator");

  auto routed_session = agent->get_or_create_session("u5", "c5");
  ASSERT_NE(routed_session, nullptr);
  EXPECT_EQ(routed_session, pooled_session);
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

  auto pooled_session = agent->get_session_by_key("0");
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

  auto session = agent->get_session_by_key("0");
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

}  // namespace tests
}  // namespace agentflow
