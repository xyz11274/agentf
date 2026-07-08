#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "agent/agent.h"
#include "agent/context.h"
#include "agent/msgque.h"
#include "agent/session.h"
#include "agent/task.h"
#include "agent/builtin_dispatcher.h"
#include "agent/session_router.h"
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

TEST_F(AgentTest, TrustedPlatformContextPropagatesToRuntimeTask) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf_platform";
  wf_cfg.name = "wf_platform";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req-platform";
  req.content = "user-input";
  req.metadata["user_id"] = "platform-user";
  req.metadata["client_id"] = "platform-client";
  req.metadata["workflow_id"] = "wf_platform";
  req.metadata["request_id"] = "request-001";
  req.metadata["platform_context"] =
      R"({"services":{"IMInferScheduler":{"status":"resolved","instances":[{"host":"10.0.0.5","port":27321}]}}})";

  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("platform-user:platform-client"), 1);
  const auto session = agent->get_session_by_key("platform-user:platform-client");
  ASSERT_NE(session, nullptr);
  const auto runtime_task = find_task_by_message_id(session->get_tasks(), "req-platform");
  ASSERT_NE(runtime_task, nullptr);

  std::string platform_context;
  std::string request_id;
  EXPECT_TRUE(runtime_task->get_parameter("platform_context", platform_context));
  EXPECT_TRUE(runtime_task->get_parameter("request_id", request_id));
  EXPECT_EQ(platform_context, req.metadata["platform_context"]);
  EXPECT_EQ(request_id, "request-001");
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
  auto dispatcher_task = session->get_task(session_id + "_dispatcher");
  auto coordinator_task = session->get_task(session_id + "_coordinator");
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
  auto default_dispatcher = session->get_task(session_id + "_dispatcher");
  auto default_coordinator = session->get_task(session_id + "_coordinator");
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

TEST_F(AgentTest, CoordinatorSkipsStaleWorkflowCompleteWhenNewerWorkflowPending) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_stale_coordinator_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "stale-coord-agent",
      "version": "1.0.0",
      "workflows": ["wf_coordinator.json"]
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
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "wf_coordinator.json" },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "wf_coordinator.json" },
            { "id": 2, "type": "skill", "description": "Dummy skill", "workflow_file": "wf_coordinator.json", "skill": { "name": "dummy", "config_dir": "skills/dummy" } }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "wf_coordinator.json", "wf_coordinator");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);
  auto session = get_or_create_first_session(agent);
  ASSERT_NE(session, nullptr);
  auto coordinator_task = session->get_coordinator_task();
  ASSERT_NE(coordinator_task, nullptr);

  // ── Simulate bug scenario ───────────────────────────────────
  // 1. Publish a "response" in DataBus (as if a skill already wrote its result)
  std::string dialogue_id = "dialogue_test_stale";
  agent->publish_data(dialogue_id + "/response", std::string("skill_execution_result_content"));

  // 2. Enqueue a stale WORKFLOW_COMPLETE data exchange message first.
  //    This simulates a completion from a PREVIOUS coordinator round that is
  //    still in the queue when a new workflow is triggered.
  framework::MMAgentMsgQue::Message stale_completion;
  stale_completion.message_id = "stale_coordinator_completion";
  stale_completion.metadata["task_type"] = "coordinator";
  stale_completion.metadata["workflow_id"] = "wf_coordinator";
  stale_completion.metadata["dialogue_id"] = dialogue_id;
  stale_completion.content = "workflow_completed";
  coordinator_task->enqueue_data_exchange_message(stale_completion);
  ASSERT_EQ(coordinator_task->get_data_exchange_queue_size(), 1);

  // 3. Enqueue a skill response + trigger to the coordinator's data_exchange queue.
  //    This simulates the dispatcher forwarding skill completion to the coordinator.
  //    When the handler processes this trigger, it dequeues the skill response and
  //    triggers a new coordinator workflow.  Before doing so, it calls
  //    clear_data_exchange_messages() to discard the stale WORKFLOW_COMPLETE.
  framework::MMAgentMsgQue::Message skill_resp;
  skill_resp.message_id = "skill_response_msg";
  skill_resp.metadata["task_type"] = "coordinator";
  skill_resp.metadata["skill_name"] = "dummy_skill";
  skill_resp.metadata["skill_success"] = "true";
  skill_resp.metadata["dialogue_id"] = dialogue_id;
  coordinator_task->enqueue_response_message(skill_resp);

  framework::MMAgentMsgQue::Message trigger;
  trigger.message_id = "trigger-skill_response_msg";
  trigger.metadata["task_type"] = "coordinator";
  trigger.metadata["trigger_type"] = "skill_response";
  trigger.metadata["dialogue_id"] = dialogue_id;
  coordinator_task->enqueue_data_exchange_message(trigger);

  // 4. Process all messages - the stale completion should be cleared
  //    when the trigger is processed (clear_data_exchange_messages).
  agent->process_all_sessions_until_idle();

  // 5. Verify the "response" key is STILL in DataBus (stale completion was
  //    cleared before it could consume the response).
  DataVariant response_after;
  bool response_still_present = agent->get_data(dialogue_id + "/response", response_after);
  EXPECT_TRUE(response_still_present)
      << "The 'response' key should still be in DataBus because the stale "
         "WORKFLOW_COMPLETE was cleared by clear_data_exchange_messages() "
         "when the new coordinator workflow was triggered.";

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

  auto dispatcher_task = pooled_session->get_task("0_dispatcher");
  auto coordinator_task = pooled_session->get_task("0_coordinator");
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
  auto dispatcher = session->get_task("0_dispatcher");
  auto coordinator = session->get_task("0_coordinator");
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
  EXPECT_EQ(skills[0].name, "search");
  EXPECT_EQ(skills[1].name, "summary");
  EXPECT_EQ(skills[2].name, "delegate");

  // Test comma-separated format
  std::string csv = "search, summary, delegate";
  value = csv;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0].name, "search");
  EXPECT_EQ(skills[1].name, "summary");
  EXPECT_EQ(skills[2].name, "delegate");

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
  EXPECT_EQ(skills[0].name, "shopping");
  EXPECT_EQ(skills[1].name, "foodie");

  // Test JSON array with spaces
  std::string with_spaces = R"([  "travel" ,  "search"  ])";
  value = with_spaces;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 2);
  EXPECT_EQ(skills[0].name, "travel");
  EXPECT_EQ(skills[1].name, "search");

  // Test JSON array with newlines
  std::string with_newlines = "[\n  \"skill1\",\n  \"skill2\"\n]";
  value = with_newlines;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 2);
  EXPECT_EQ(skills[0].name, "skill1");
  EXPECT_EQ(skills[1].name, "skill2");

  // Test single element JSON array
  std::string single = R"(["only_one"])";
  value = single;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 1);
  EXPECT_EQ(skills[0].name, "only_one");

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
  EXPECT_EQ(skills[0].name, "first");
  EXPECT_EQ(skills[1].name, "second");
  EXPECT_EQ(skills[2].name, "third");
}

// Test new SkillSpec format with exec_skill_key
TEST_F(AgentTest, BuiltinDispatcherParsesNextSkillsWithExecSkillKey) {
  // Test new object format with exec_skill_key
  std::string json_with_keys = R"([
    {"name": "travel", "exec_skill_key": "travel_data"},
    {"name": "shopping", "exec_skill_key": "shopping_data"},
    {"name": "foodie"}
  ])";
  DataVariant value = json_with_keys;
  auto skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0].name, "travel");
  EXPECT_EQ(skills[0].exec_skill_key, "travel_data");
  EXPECT_EQ(skills[1].name, "shopping");
  EXPECT_EQ(skills[1].exec_skill_key, "shopping_data");
  EXPECT_EQ(skills[2].name, "foodie");
  EXPECT_EQ(skills[2].exec_skill_key, "");  // Optional field, should be empty

  // Test mixed format: string and object
  std::string mixed_format = R"([
    "skill1",
    {"name": "skill2", "exec_skill_key": "skill2_data"},
    "skill3"
  ])";
  value = mixed_format;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0].name, "skill1");
  EXPECT_EQ(skills[0].exec_skill_key, "");
  EXPECT_EQ(skills[1].name, "skill2");
  EXPECT_EQ(skills[1].exec_skill_key, "skill2_data");
  EXPECT_EQ(skills[2].name, "skill3");
  EXPECT_EQ(skills[2].exec_skill_key, "");
}

TEST_F(AgentTest, BuiltinDispatcherParsesNextSkillsWithForceField) {
  // Test new object format with force field for re-launch
  std::string json_with_force = R"([
    {"name": "travel", "exec_skill_key": "travel_data", "force": true},
    {"name": "shopping", "force": true},
    {"name": "foodie"}
  ])";
  DataVariant value = json_with_force;
  auto skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0].name, "travel");
  EXPECT_EQ(skills[0].exec_skill_key, "travel_data");
  EXPECT_TRUE(skills[0].force);  // Force re-launch enabled
  EXPECT_EQ(skills[1].name, "shopping");
  EXPECT_EQ(skills[1].exec_skill_key, "");
  EXPECT_TRUE(skills[1].force);  // Force re-launch enabled
  EXPECT_EQ(skills[2].name, "foodie");
  EXPECT_EQ(skills[2].exec_skill_key, "");
  EXPECT_FALSE(skills[2].force);  // Default false, no force

  // Test mixed format with force field
  std::string mixed_with_force = R"([
    "skill1",
    {"name": "skill2", "exec_skill_key": "skill2_data", "force": true},
    {"name": "skill3", "force": false}
  ])";
  value = mixed_with_force;
  skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0].name, "skill1");
  EXPECT_EQ(skills[0].exec_skill_key, "");
  EXPECT_FALSE(skills[0].force);  // String format defaults to false
  EXPECT_EQ(skills[1].name, "skill2");
  EXPECT_EQ(skills[1].exec_skill_key, "skill2_data");
  EXPECT_TRUE(skills[1].force);
  EXPECT_EQ(skills[2].name, "skill3");
  EXPECT_EQ(skills[2].exec_skill_key, "");
  EXPECT_FALSE(skills[2].force);
}

TEST_F(AgentTest, BuiltinDispatcherResetForceFlags) {
  // Test resetting force flags for specified skills
  std::string json_with_force = R"([
    {"name": "travel", "exec_skill_key": "travel_data", "force": true},
    {"name": "shopping", "force": true},
    {"name": "foodie"}
  ])";

  // Reset force for "travel" only
  std::vector<std::string> skills_to_reset = {"travel"};
  std::string updated = framework::BuiltinDispatcher::reset_force_flags(json_with_force, skills_to_reset);

  // Parse the updated JSON to verify
  DataVariant value = updated;
  auto skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0].name, "travel");
  EXPECT_FALSE(skills[0].force);  // Reset to false
  EXPECT_EQ(skills[0].exec_skill_key, "travel_data");  // Other fields preserved
  EXPECT_EQ(skills[1].name, "shopping");
  EXPECT_TRUE(skills[1].force);  // Still true (not in reset list)
  EXPECT_EQ(skills[2].name, "foodie");
  EXPECT_FALSE(skills[2].force);  // Default false

  // Reset force for multiple skills
  std::string json_multi_force = R"([
    {"name": "travel", "force": true},
    {"name": "shopping", "force": true},
    {"name": "foodie", "force": true}
  ])";
  skills_to_reset = {"travel", "shopping", "foodie"};
  updated = framework::BuiltinDispatcher::reset_force_flags(json_multi_force, skills_to_reset);
  skills = framework::BuiltinDispatcher::parse_next_skills(updated);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_FALSE(skills[0].force);
  EXPECT_FALSE(skills[1].force);
  EXPECT_FALSE(skills[2].force);

  // Test empty skill list (no change)
  skills_to_reset = {};
  updated = framework::BuiltinDispatcher::reset_force_flags(json_multi_force, skills_to_reset);
  EXPECT_EQ(updated, json_multi_force);  // Unchanged

  // Test skill not in JSON (no change)
  skills_to_reset = {"nonexistent"};
  updated = framework::BuiltinDispatcher::reset_force_flags(json_multi_force, skills_to_reset);
  skills = framework::BuiltinDispatcher::parse_next_skills(updated);
  EXPECT_TRUE(skills[0].force);  // Still true (skill not found)
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
      "session_pool_size": 1,
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

  auto dispatcher = session->get_task("0_dispatcher");
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
      "session_pool_size": 1,
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
      "session_pool_size": 1,
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
      "session_pool_size": 1,
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
      "session_pool_size": 1,
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

// ============================================================
// candidate_skills Tests
// ============================================================

// Helper function for candidate_skills tests (defined in anonymous namespace in builtin_dispatcher.cpp)
namespace {
bool test_validate_skill_against_candidates(const std::string& skill_name,
                                            const std::vector<std::string>& candidate_skills) {
  if(candidate_skills.empty()) {
    return true;
  }
  return std::find(candidate_skills.begin(), candidate_skills.end(), skill_name) != candidate_skills.end();
}
}

class CandidateSkillsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    agent = std::make_shared<framework::MMAgent>(base_config());
  }

  framework::MMAgent::Config base_config() {
    framework::MMAgent::Config config;
    config.id = "test_agent";
    config.name = "Test Agent";
    return config;
  }

  std::shared_ptr<framework::MMAgent> agent;
};

TEST_F(CandidateSkillsTest, NodeConfigHasCandidateSkillsField) {
  framework::MMNode::Config config;
  config.id = "test_node";
  config.name = "Test Node";

  // Verify field exists and defaults to empty
  EXPECT_TRUE(config.candidate_skills.empty());

  // Verify we can add skills
  config.candidate_skills = {"search", "translate", "summarize"};
  EXPECT_EQ(config.candidate_skills.size(), 3);
  EXPECT_EQ(config.candidate_skills[0], "search");
  EXPECT_EQ(config.candidate_skills[1], "translate");
  EXPECT_EQ(config.candidate_skills[2], "summarize");
}

TEST_F(CandidateSkillsTest, NodeGetterReturnsCandidateSkills) {
  framework::MMNode::Config config;
  config.id = "test_node";
  config.candidate_skills = {"skill1", "skill2"};

  auto node = std::make_shared<framework::MMNode>(config);
  const auto& skills = node->get_candidate_skills();

  EXPECT_EQ(skills.size(), 2);
  EXPECT_EQ(skills[0], "skill1");
  EXPECT_EQ(skills[1], "skill2");
}

TEST_F(CandidateSkillsTest, ParseCandidateSkillsFromJson) {
  // Test JSON array format
  std::string json_array = R"(["search", "translate", "summarize"])";
  DataVariant value = json_array;

  auto skills = framework::BuiltinDispatcher::parse_next_skills(value);
  EXPECT_EQ(skills.size(), 3);
  EXPECT_EQ(skills[0].name, "search");
  EXPECT_EQ(skills[1].name, "translate");
  EXPECT_EQ(skills[2].name, "summarize");
}

TEST_F(CandidateSkillsTest, EmptyCandidateSkillsAllowsAll) {
  // Empty candidate_skills means no restrictions
  std::vector<std::string> empty_candidates;

  // Any skill should be allowed when candidates is empty
  EXPECT_TRUE(test_validate_skill_against_candidates("any_skill", empty_candidates));
  EXPECT_TRUE(test_validate_skill_against_candidates("another_skill", empty_candidates));
}

TEST_F(CandidateSkillsTest, CandidateSkillsRestrictAccess) {
  std::vector<std::string> candidates = {"search", "translate", "summarize"};

  // Skills in the list should be allowed
  EXPECT_TRUE(test_validate_skill_against_candidates("search", candidates));
  EXPECT_TRUE(test_validate_skill_against_candidates("translate", candidates));
  EXPECT_TRUE(test_validate_skill_against_candidates("summarize", candidates));

  // Skills not in the list should be blocked
  EXPECT_FALSE(test_validate_skill_against_candidates("unknown_skill", candidates));
  EXPECT_FALSE(test_validate_skill_against_candidates("hack", candidates));
}

// =============================================================================
// DataBus Cleanup Tests
// =============================================================================

class DataBusCleanupTest : public ::testing::Test {
protected:
  void SetUp() override {
    databus_ = std::make_unique<runtime::MMDataBus>(1024 * 1024);
    framework::MMAgent::Config config;
    config.id = "test-agent";
    agent_ = std::make_unique<framework::MMAgent>(config);
    agent_->set_runtime_databus(databus_.get());
  }

  std::unique_ptr<runtime::MMDataBus> databus_;
  std::unique_ptr<framework::MMAgent> agent_;
};

TEST_F(DataBusCleanupTest, ClearSessionData) {
  // Put test data for different sessions
  databus_->put("/session/s1/sys/key1", DataVariant("value1"));
  databus_->put("/session/s1/user/key2", DataVariant("value2"));
  databus_->put("/session/s1/ctx/key3", DataVariant("value3"));
  databus_->put("/session/s2/sys/key4", DataVariant("value4"));

  // Verify data exists
  DataVariant value;
  EXPECT_TRUE(databus_->get("/session/s1/sys/key1", value));
  EXPECT_TRUE(databus_->get("/session/s2/sys/key4", value));

  // Clear session s1
  int count = agent_->clear_session_data("s1");

  // Should have removed 3 keys from s1
  EXPECT_EQ(count, 3);

  // s1 data should be gone
  EXPECT_FALSE(databus_->get("/session/s1/sys/key1", value));
  EXPECT_FALSE(databus_->get("/session/s1/user/key2", value));
  EXPECT_FALSE(databus_->get("/session/s1/ctx/key3", value));

  // s2 data should still exist
  EXPECT_TRUE(databus_->get("/session/s2/sys/key4", value));
}

TEST_F(DataBusCleanupTest, ClearDialogueData) {
  // Put test data for different dialogues in all 5 namespaces
  databus_->put("/session/s1/sys/d1/key1", DataVariant("value1"));
  databus_->put("/session/s1/sys/d1/key2", DataVariant("value2"));
  databus_->put("/session/s1/user/d1/key3", DataVariant("value3"));
  databus_->put("/session/s1/ctx/d1/key4", DataVariant("value4"));
  databus_->put("/session/s1/llm/d1/key5", DataVariant("value5"));
  databus_->put("/session/s1/tool/d1/key6", DataVariant("value6"));
  databus_->put("/session/s1/sys/d2/key7", DataVariant("value7"));
  databus_->put("/session/s1/user/d2/key8", DataVariant("value8"));

  // Clear dialogue d1
  int count = agent_->clear_dialogue_data("s1", "d1");

  // Should have removed 6 keys from d1 (sys, user, ctx, llm, tool namespaces)
  EXPECT_EQ(count, 6);

  // d1 data should be gone
  DataVariant value;
  EXPECT_FALSE(databus_->get("/session/s1/sys/d1/key1", value));
  EXPECT_FALSE(databus_->get("/session/s1/user/d1/key3", value));
  EXPECT_FALSE(databus_->get("/session/s1/ctx/d1/key4", value));
  EXPECT_FALSE(databus_->get("/session/s1/llm/d1/key5", value));
  EXPECT_FALSE(databus_->get("/session/s1/tool/d1/key6", value));

  // d2 data should still exist
  EXPECT_TRUE(databus_->get("/session/s1/sys/d2/key7", value));
  EXPECT_TRUE(databus_->get("/session/s1/user/d2/key8", value));
}

TEST_F(DataBusCleanupTest, ClearSessionDataNoDatabus) {
  // Test when no databus is set
  framework::MMAgent::Config config;
  config.id = "test-agent-no-databus";
  auto agent_no_databus = std::make_unique<framework::MMAgent>(config);
  // Don't set runtime databus

  int count = agent_no_databus->clear_session_data("s1");
  EXPECT_EQ(count, 0);  // Should return 0 when no databus
}

TEST_F(DataBusCleanupTest, ClearDialogueDataNoDatabus) {
  // Test when no databus is set
  framework::MMAgent::Config config;
  config.id = "test-agent-no-databus";
  auto agent_no_databus = std::make_unique<framework::MMAgent>(config);
  // Don't set runtime databus

  int count = agent_no_databus->clear_dialogue_data("s1", "d1");
  EXPECT_EQ(count, 0);  // Should return 0 when no databus
}

TEST_F(DataBusCleanupTest, ClearNonExistentSession) {
  // Clear a session that doesn't exist - should return 0
  int count = agent_->clear_session_data("nonexistent");
  EXPECT_EQ(count, 0);
}

TEST_F(DataBusCleanupTest, ClearNonExistentDialogue) {
  // Clear a dialogue that doesn't exist - should return 0
  int count = agent_->clear_dialogue_data("s1", "nonexistent");
  EXPECT_EQ(count, 0);
}

// =============================================================================
// Coordinator Progress Push Test
// =============================================================================
//
// Verifies that when the coordinator's workflow completes AND a "response" key
// exists alongside "next_skills", the framework pushes a non-final progress
// notification to the agent's response queue BEFORE the final response.
//
// Invariant: non-final progress MUST NOT trigger cleanup_dialogue_data or
// DIALOGUE_DESTROYED, and the final response MUST still be deliverable.

TEST_F(AgentTest, CoordinatorPushesNonFinalProgressBeforeFinalResponse) {
  // 1. Create agent with a coordinator workflow
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf_coord";
  wf_cfg.name = "wf_coord";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));

  ASSERT_TRUE(agent->initialize());
  ASSERT_TRUE(agent->start());

  // 2. Route a message to instantiate a session (creates dispatcher+coordinator tasks)
  framework::MMAgentMsgQue::Message req;
  req.message_id = "progress-test-msg";
  req.content = "test request";
  req.metadata["user_id"] = "progress-user";
  req.metadata["client_id"] = "progress-client";
  req.metadata["task_type"] = "dispatcher";
  req.metadata["workflow_id"] = "wf_coord";
  ASSERT_TRUE(agent->route_request_message(req));

  // Wait for session creation with coordinator task
  std::shared_ptr<framework::MMAgentSession> session;
  for (int i = 0; i < 50; ++i) {
    session = agent->get_session_by_key("progress-user:progress-client");
    if (session) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_NE(session, nullptr);
  std::string sid = session->get_session_id();

  // 3. Find the coordinator task
  auto tasks = session->get_tasks();
  auto coordinator_task = find_task_by_type(tasks, "coordinator");
  ASSERT_NE(coordinator_task, nullptr) << "Session must have a coordinator task";

  // 4. Pre-populate launched_skills so process_next_skills_launch sees
  //    all skills as launched (avoids needing a real skill workflow template).
  agent->publish_data("test-dialogue-progress/launched_skills",
                      DataVariant(std::string("test-skill")));
  agent->publish_data("test-dialogue-progress/skills_scheduled",
                      DataVariant(std::string("true")));

  // Write response and next_skills to agent's internal data store
  // (simulating what coordinator_decide.py outputs to the DataBus)
  std::string dialogue_id = "test-dialogue-progress";
  std::string response_val = "正在执行 test-skill…";
  std::string next_skills_val = R"([{"name":"test-skill","prompt":"do work"}])";

  agent->publish_data("/session/" + sid + "/sys/" + dialogue_id + "/response",
                      DataVariant(response_val));
  agent->publish_data("/session/" + sid + "/sys/" + dialogue_id + "/next_skills",
                      DataVariant(next_skills_val));

  // 5. Enqueue a data_exchange message directly on the coordinator task
  //    to trigger COORDINATORDataExchange handler (bypassing dispatcher routing)
  framework::MMAgentMsgQue::Message trigger_msg;
  trigger_msg.message_id = "coord-trigger-progress";
  trigger_msg.metadata["dialogue_id"] = dialogue_id;
  trigger_msg.metadata["task_type"] = "coordinator";
  trigger_msg.metadata["message_type"] = "data_exchange";
  ASSERT_TRUE(coordinator_task->enqueue_data_exchange_message(trigger_msg));

  // 6. Wait for the coordinator's data_exchange_loop to process the message
  //    and push a non-final progress notification to the session router's queue.
  framework::MMAgentMsgQue::Message progress_msg;
  bool got_progress = false;
  for (int i = 0; i < 100; ++i) {
    if (agent->dequeue_response_message(progress_msg)) {
      got_progress = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // RED: this FAILS because no non-final progress is pushed before the fix.
  //      The only way a response appears is via the "Response ready" path
  //      which marks final=true and triggers cleanup.
  // GREEN: after the fix, the progress notification IS pushed.
  EXPECT_TRUE(got_progress)
      << "Non-final progress notification should be pushed when response key "
         "exists alongside next_skills";

  if (got_progress) {
    // Verify the message is NON-FINAL
    auto final_it = progress_msg.metadata.find("final");
    bool is_final = (final_it != progress_msg.metadata.end() && final_it->second == "true");
    EXPECT_FALSE(is_final) << "Progress must be non-final (final=false)";

    // Verify content contains progress text
    EXPECT_TRUE(progress_msg.content.find("正在执行") != std::string::npos);
  }

  agent->stop();
}

// =============================================================================
// Coordinator Multi-Round Loop Tests
// =============================================================================
//
// Tests the skill-completion → coordinator-re-trigger path.
// Round 1: coordinator dispatches skills, pushes progress.
// Skill completes → C++ decrements pending_skills_count → re-triggers coordinator.
// Round 2: coordinator outputs final response (no next_skills).
//
// Invariant: skill completion must re-trigger coordinator via intent_request.

TEST_F(AgentTest, CoordinatorSkillCompletionTriggersReTrigger) {
  // 1. Create agent with a coordinator workflow
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf_coord";
  wf_cfg.name = "wf_coord";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));

  ASSERT_TRUE(agent->initialize());
  ASSERT_TRUE(agent->start());

  // 2. Route a message to instantiate a session
  framework::MMAgentMsgQue::Message req;
  req.message_id = "multi-round-test-msg";
  req.content = "test request";
  req.metadata["user_id"] = "multi-round-user";
  req.metadata["client_id"] = "multi-round-client";
  req.metadata["task_type"] = "dispatcher";
  req.metadata["workflow_id"] = "wf_coord";
  ASSERT_TRUE(agent->route_request_message(req));

  // Wait for session creation
  std::shared_ptr<framework::MMAgentSession> session;
  for (int i = 0; i < 50; ++i) {
    session = agent->get_session_by_key("multi-round-user:multi-round-client");
    if (session) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_NE(session, nullptr);
  std::string sid = session->get_session_id();

  // 3. Find the coordinator task
  auto tasks = session->get_tasks();
  auto coordinator_task = find_task_by_type(tasks, "coordinator");
  ASSERT_NE(coordinator_task, nullptr) << "Session must have a coordinator task";

  // 4. Set up databus for round 1: pre-populate pending_skills_count and launched_skills
  std::string dialogue_id = "test-multi-round";
  agent->publish_data(dialogue_id + "/pending_skills_count", DataVariant(static_cast<int64_t>(1)));
  agent->publish_data(dialogue_id + "/launched_skills",
                      DataVariant(std::string("test-skill")));
  agent->publish_data(dialogue_id + "/skills_scheduled",
                      DataVariant(std::string("true")));

  // Write next_skills and response (simulating coordinator_decide.py output)
  std::string response_val = "正在执行 test-skill…";
  std::string next_skills_val = R"([{"name":"test-skill","prompt":"do work"}])";
  agent->publish_data("/session/" + sid + "/sys/" + dialogue_id + "/response",
                      DataVariant(response_val));
  agent->publish_data("/session/" + sid + "/sys/" + dialogue_id + "/next_skills",
                      DataVariant(next_skills_val));

  // 5. Trigger round 1: send data_exchange to coordinator to process next_skills
  framework::MMAgentMsgQue::Message trigger_msg;
  trigger_msg.message_id = "coord-trigger-round1";
  trigger_msg.metadata["dialogue_id"] = dialogue_id;
  trigger_msg.metadata["task_type"] = "coordinator";
  ASSERT_TRUE(coordinator_task->enqueue_data_exchange_message(trigger_msg));

  // 6. Verify round 1: progress notification is pushed
  framework::MMAgentMsgQue::Message progress_msg;
  bool got_progress = false;
  for (int i = 0; i < 100; ++i) {
    if (agent->dequeue_response_message(progress_msg)) {
      got_progress = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(got_progress)
      << "Round 1: Non-final progress notification should be pushed";
  if (got_progress) {
    auto final_it = progress_msg.metadata.find("final");
    bool is_final = (final_it != progress_msg.metadata.end() && final_it->second == "true");
    EXPECT_FALSE(is_final) << "Round 1: Progress must be non-final (final=false)";
  }

  // 7. Simulate skill completion: send workflow_completed with skill metadata
  //    to the coordinator task's data_exchange queue.
  //    The handler detects skill completion (lines 1519-1533), decrements the
  //    pending_skills_count and re-triggers coordinator via intent_request.
  framework::MMAgentMsgQue::Message skill_complete_msg;
  skill_complete_msg.message_id = "skill-complete-test-skill";
  skill_complete_msg.content = "workflow_completed";
  skill_complete_msg.metadata["task_type"] = "skill";
  skill_complete_msg.metadata["skill_name"] = "test-skill";
  skill_complete_msg.metadata["dialogue_id"] = dialogue_id;
  ASSERT_TRUE(coordinator_task->enqueue_data_exchange_message(skill_complete_msg));

  // 8. Wait briefly for processing
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 9. Verify pending_skills_count was decremented to 0
  DataVariant pending_count_value;
  bool found_pending = agent->get_data(dialogue_id + "/pending_skills_count",
                                        pending_count_value);
  EXPECT_TRUE(found_pending) << "pending_skills_count should still exist";
  if (found_pending) {
    if (const auto* count_ptr = std::get_if<int64_t>(&pending_count_value)) {
      EXPECT_EQ(*count_ptr, 0) << "pending_skills_count should be 0 after skill completion";
    }
  }

  // 10. Verify coordinator was re-triggered after skill completion.
  //     The re-trigger creates a new runtime_task via build_runtime_task →
  //     session->add_task() → session task count increases.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto tasks_after = session->get_tasks();
  // The re-trigger should have created 1 additional runtime task
  int new_task_count = static_cast<int>(tasks_after.size());
  EXPECT_GE(new_task_count, static_cast<int>(tasks.size()) + 1)
      << "Session must have at least " << (tasks.size() + 1)
      << " tasks after skill completion re-trigger (was " << tasks.size() << ")";

  // 11. Verify skill_failed flag was NOT written (skill succeeded)
  DataVariant skill_failed_value;
  std::string skill_failed_key = "/session/" + sid + "/sys/"
      + dialogue_id + "/" + "skill_failed";
  bool found_failed = agent->get_data(skill_failed_key, skill_failed_value);
  EXPECT_FALSE(found_failed)
      << "skill_failed flag should NOT exist for successful skill";

  agent->stop();
}

// =============================================================================
// Skill Failure → skill_failed Flag Test
// =============================================================================
//
// When a skill task fails (workflow_failed), the C++ handler must write
// skill_failed=true to the databus and still re-trigger the coordinator.
// This flag is consumed by the Python coordinator_decide.py.

TEST_F(AgentTest, SkillFailureWritesSkillFailedFlag) {
  // 1. Create agent with a coordinator workflow
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf_coord";
  wf_cfg.name = "wf_coord";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));

  ASSERT_TRUE(agent->initialize());
  ASSERT_TRUE(agent->start());

  // 2. Route a message to instantiate a session
  framework::MMAgentMsgQue::Message req;
  req.message_id = "skill-fail-test-msg";
  req.content = "test request";
  req.metadata["user_id"] = "skill-fail-user";
  req.metadata["client_id"] = "skill-fail-client";
  req.metadata["task_type"] = "dispatcher";
  req.metadata["workflow_id"] = "wf_coord";
  ASSERT_TRUE(agent->route_request_message(req));

  // Wait for session creation
  std::shared_ptr<framework::MMAgentSession> session;
  for (int i = 0; i < 50; ++i) {
    session = agent->get_session_by_key("skill-fail-user:skill-fail-client");
    if (session) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_NE(session, nullptr);
  std::string sid = session->get_session_id();

  // 3. Find the coordinator task
  auto tasks = session->get_tasks();
  auto coordinator_task = find_task_by_type(tasks, "coordinator");
  ASSERT_NE(coordinator_task, nullptr) << "Session must have a coordinator task";

  std::string dialogue_id = "test-skill-fail";

  // 4. Set up pending_skills_count so it can be decremented
  agent->publish_data(dialogue_id + "/pending_skills_count", DataVariant(static_cast<int64_t>(1)));

  // 5. Send workflow_failed message simulating skill failure
  framework::MMAgentMsgQue::Message skill_fail_msg;
  skill_fail_msg.message_id = "skill-fail-failed-skill";
  skill_fail_msg.content = "workflow_failed";
  skill_fail_msg.metadata["task_type"] = "skill";
  skill_fail_msg.metadata["skill_name"] = "failed-skill";
  skill_fail_msg.metadata["dialogue_id"] = dialogue_id;
  ASSERT_TRUE(coordinator_task->enqueue_data_exchange_message(skill_fail_msg));

  // 6. Wait briefly for processing
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 7. Verify skill_failed=true was written to databus
  DataVariant skill_failed_value;
  std::string skill_failed_key = "/session/" + sid + "/sys/"
      + dialogue_id + "/" + "skill_failed";
  bool found_failed = agent->get_data(skill_failed_key, skill_failed_value);
  EXPECT_TRUE(found_failed) << "skill_failed flag must be written to databus";
  if (found_failed) {
    if (const auto* text = std::get_if<std::string>(&skill_failed_value)) {
      EXPECT_EQ(*text, "true") << "skill_failed must equal 'true'";
    }
  }

  // 8. Verify pending_skills_count was decremented
  DataVariant pending_count_value;
  bool found_pending = agent->get_data(dialogue_id + "/pending_skills_count",
                                        pending_count_value);
  EXPECT_TRUE(found_pending) << "pending_skills_count should still exist";
  if (found_pending) {
    if (const auto* count_ptr = std::get_if<int64_t>(&pending_count_value)) {
      EXPECT_EQ(*count_ptr, 0) << "pending_skills_count should be 0 after skill failure";
    }
  }

  // 9. Verify coordinator was re-triggered even for failed skill.
  //     The re-trigger creates a new runtime_task via build_runtime_task →
  //     session->add_task() → session task count increases.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto tasks_after = session->get_tasks();
  int new_task_count = static_cast<int>(tasks_after.size());
  EXPECT_GE(new_task_count, static_cast<int>(tasks.size()) + 1)
      << "Session must have at least " << (tasks.size() + 1)
      << " tasks after skill failure re-trigger (was " << tasks.size() << ")";

  agent->stop();
}

// ════════════════════════════════════════════════════════════════════
// Dialogue serialization lock tests (session-level)
// ════════════════════════════════════════════════════════════════════

class DialogueSerialLockTest : public ::testing::Test {
 protected:
  framework::MMAgentSession::Config session_cfg() {
    framework::MMAgentSession::Config cfg;
    cfg.session_id = "test-ses-serial";
    cfg.max_queue_size = 16;
    return cfg;
  }
};

TEST_F(DialogueSerialLockTest, AcquireReleasesOnGuardDestroy) {
  framework::MMAgentSession ses(session_cfg());
  EXPECT_EQ(ses.get_active_dialogue_id(), "");

  // Acquire and immediately release by destroying the guard.
  {
    auto g = ses.acquire_dialogue_lock("d1");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(ses.get_active_dialogue_id(), "d1");
    EXPECT_FALSE(g->is_cancelled());
    ses.hold_dialogue_lock("d1", std::move(g));
  }
  // Slot still held (held_locks_ owns it).
  EXPECT_EQ(ses.get_active_dialogue_id(), "d1");

  ses.release_dialogue_lock("d1");
  EXPECT_EQ(ses.get_active_dialogue_id(), "");
}

TEST_F(DialogueSerialLockTest, SecondAcquireBlocksUntilFirstReleases) {
  framework::MMAgentSession ses(session_cfg());

  ses.hold_dialogue_lock("d1", ses.acquire_dialogue_lock("d1"));
  EXPECT_EQ(ses.get_active_dialogue_id(), "d1");

  // Launch a thread that tries to acquire d2 — should block.
  std::atomic<bool> got_d2{false};
  std::thread waiter([&]() {
    auto g2 = ses.acquire_dialogue_lock("d2");
    if (g2) {
      ses.hold_dialogue_lock("d2", std::move(g2));
      got_d2.store(true);
    }
  });

  // Give the waiter time to block on the cv.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(got_d2.load()) << "d2 must block while d1 holds the lock";
  EXPECT_EQ(ses.get_active_dialogue_id(), "d1");

  // Release d1 — waiter should wake and acquire.
  ses.release_dialogue_lock("d1");
  waiter.join();
  EXPECT_TRUE(got_d2.load()) << "d2 must acquire after d1 released";
  EXPECT_EQ(ses.get_active_dialogue_id(), "d2");

  ses.release_dialogue_lock("d2");
  EXPECT_EQ(ses.get_active_dialogue_id(), "");
}

TEST_F(DialogueSerialLockTest, CancelAllDialoguesWakesBlockedWaiters) {
  framework::MMAgentSession ses(session_cfg());
  ses.hold_dialogue_lock("d1", ses.acquire_dialogue_lock("d1"));

  std::atomic<bool> waiter_returned{false};
  std::thread waiter([&]() {
    auto g2 = ses.acquire_dialogue_lock("d2");
    // After cancel_all_dialogues, acquire must return nullptr.
    EXPECT_EQ(g2, nullptr);
    waiter_returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(waiter_returned.load());

  ses.cancel_all_dialogues();
  waiter.join();
  EXPECT_TRUE(waiter_returned.load()) << "blocked waiter must wake on cancel";
  // active_dialogue_id_ cleared by ~DialogueLockGuard of the held d1 lock.
  EXPECT_EQ(ses.get_active_dialogue_id(), "");
}

TEST_F(DialogueSerialLockTest, ReleaseUnknownDialogueIdIsIdempotent) {
  framework::MMAgentSession ses(session_cfg());
  // Releasing a dialogue that was never locked must not throw or deadlock.
  ses.release_dialogue_lock("nonexistent");
  EXPECT_EQ(ses.get_active_dialogue_id(), "");
}

TEST_F(DialogueSerialLockTest, DoubleReleaseIsSafe) {
  framework::MMAgentSession ses(session_cfg());
  ses.hold_dialogue_lock("d1", ses.acquire_dialogue_lock("d1"));
  ses.release_dialogue_lock("d1");
  EXPECT_EQ(ses.get_active_dialogue_id(), "");
  // Second release must be a no-op.
  ses.release_dialogue_lock("d1");
  EXPECT_EQ(ses.get_active_dialogue_id(), "");
}

// ============================================================================
// P0-2 白盒测试：锁住 handle_session_destroy 路径的事件时序不变量
//   handle_session_destroy 是 framework 内部 SESSION_DESTROY 控制指令触发，
//   e2e 黑盒难直接触发。此处通过 route_request_message + MessageType::CONTROL
//   + content=SESSION-DESTROY 走真实路径，白盒验证事件时序。
//   不变量：
//     I-1  SESSION_DESTROYED 事件发射时，session 的 tasks 已清空（count=0）
//     I-2  SESSION_DESTROYED 事件发射时，session 的 dialogue locks 已全部释放
//     I-3  databus 数据已清（handle_session_destroy 复用 clear_session_data_internal）
// ============================================================================

TEST_F(AgentTest, SessionDestroyEventFiresAfterTasksAndDialoguesCleared) {
  // 构造 agent 并注入 databus
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  auto databus = std::make_unique<agentflow::runtime::MMDataBus>(1024 * 1024);
  ASSERT_TRUE(agent->initialize());
  ASSERT_TRUE(agent->start());
  agent->set_runtime_databus(databus.get());

  // 1. Route 一个 intent_request 触发 session 创建（含 dispatcher+coordinator tasks）
  framework::MMAgentMsgQue::Message req;
  req.message_id = "p0-2-setup-msg";
  req.metadata["message_type"] = "intent_request";
  req.metadata["user_id"]     = "p0-2-user";
  req.metadata["client_id"]   = "p0-2-client";
  ASSERT_TRUE(agent->route_request_message(req));
  // 等待 session 路由完成
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  agent->process_all_sessions_until_idle();

  auto sessions = agent->get_sessions();
  ASSERT_FALSE(sessions.empty());
  auto session = sessions.front();
  ASSERT_NE(session, nullptr);
  const std::string sid = session->get_session_id();

  // 前置：session 应有 tasks（dispatcher + coordinator 至少 2 个）
  EXPECT_GE(session->get_tasks().size(), 2u) << "前置：session 应有 dispatcher+coordinator tasks";

  // 2. 注入 session_event_callback，在事件回调内观测 tasks 数与 dialogue lock 状态
  std::atomic<int>  tasks_at_event{-1};
  std::atomic<bool> dialogue_held_at_event{true};
  std::atomic<bool> event_fired{false};
  std::weak_ptr<framework::MMAgentSession> session_ref = session;

  agent->set_session_event_callback(
      [&](const std::string&, const std::string&, const std::string&,
          framework::SessionEventType evt) {
        if(evt == framework::SessionEventType::DESTROYED) {
          auto sp = session_ref.lock();
          if(sp) {
            tasks_at_event          = static_cast<int>(sp->get_tasks().size());
            dialogue_held_at_event  = !sp->get_active_dialogue_id().empty();
          }
          event_fired = true;
        }
      });

  // 3. 写入 databus 数据，让 handle_session_destroy 有东西可清
  databus->put("/session/" + sid + "/sys/k", DataVariant(std::string("v")));

  // 4. 发送 SESSION-DESTROY 控制指令走真实路径
  framework::MMAgentMsgQue::Message destroy_cmd;
  destroy_cmd.message_id = "p0-2-destroy";
  destroy_cmd.type       = framework::MMAgentMsgQue::MessageType::CONTROL;
  destroy_cmd.content    = framework::MMAgentMsgQue::ControlCommand::SESSION_DESTROY;
  destroy_cmd.metadata["user_id"]    = "p0-2-user";
  destroy_cmd.metadata["client_id"]  = "p0-2-client";
  ASSERT_TRUE(agent->route_request_message(destroy_cmd));
  // 等待控制指令处理完成
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  agent->process_all_sessions_until_idle();

  // 5. 不变量验证
  EXPECT_TRUE(event_fired) << "前置：SESSION-DESTROY 应触发 DESTROYED 事件";

  // I-1：事件发射时 tasks 已清空（handle_session_destroy 第 4 步 clear_all_tasks 在发事件前）
  EXPECT_EQ(tasks_at_event.load(), 0)
      << "I-1：SESSION_DESTROYED 事件发射时 session 的 tasks 应已清空";

  // I-2：事件发射时 dialogue locks 已释放（handle_session_destroy 第 5 步 cancel_all_dialogues 在发事件前）
  EXPECT_FALSE(dialogue_held_at_event.load())
      << "I-2：SESSION_DESTROYED 事件发射时 dialogue locks 应已全部释放";

  // I-3：databus 数据已清（handle_session_destroy 复用 clear_session_data_internal）
  EXPECT_FALSE(databus->contains("/session/" + sid + "/sys/k"))
      << "I-3：handle_session_destroy 应清掉 databus 数据";

  agent->clear_session_event_callback();
  agent->stop();
}

}  // namespace tests
}  // namespace agentflow
