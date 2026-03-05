#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "agent/agent.h"
#include "agent/msgque.h"
#include "agent/session.h"
#include "agent/task.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace tests {

namespace {
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
  output.metadata["message_type"] = "output_result";
  ASSERT_TRUE(queue.enqueue_message(output));

  framework::MMAgentMsgQue::Message out;
  ASSERT_TRUE(queue.dequeue_message_by_type(framework::MMAgentMsgQue::MessageQueueType::OUTPUT_RESULT, out));
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

TEST_F(AgentTest, RouteRequestCreatesSessionByUserAndClient) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req1";
  req.metadata["user_id"] = "u1";
  req.metadata["client_id"] = "c1";
  ASSERT_TRUE(agent->route_request_message(req));

  auto session = agent->get_session_by_key("u1:c1");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->get_receive_queue_size(), 1);
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
  ASSERT_EQ(tasks.size(), 3);
  auto collaboration_task = find_task_by_type(tasks, "collaboration");
  ASSERT_NE(collaboration_task, nullptr);
  EXPECT_EQ(collaboration_task->get_session_id(), "u2:c2");
  EXPECT_EQ(collaboration_task->get_workflow_id(), "wf1");
  EXPECT_EQ(collaboration_task->get_workflow_file(), "wf1.json");
  EXPECT_EQ(collaboration_task->get_collaboration_so_path(), "/tmp/demo_collab.so");
  ASSERT_NE(collaboration_task->get_context(), nullptr);
  std::string message_content;
  std::string message_data_ref;
  EXPECT_TRUE(collaboration_task->get_parameter("message_content", message_content));
  EXPECT_TRUE(collaboration_task->get_parameter("message_data_ref", message_data_ref));
  EXPECT_EQ(message_content, "task-input");
  EXPECT_FALSE(message_data_ref.empty());
}

TEST_F(AgentTest, CoordinatorDefaultsToTask0Type) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  framework::MMWorkflow::Config wf_cfg;
  wf_cfg.id = "wf_default";
  wf_cfg.name = "wf_default";
  agent->add_workflow(std::make_shared<framework::MMWorkflow>(wf_cfg));

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req_task0";
  req.metadata["user_id"] = "u3";
  req.metadata["client_id"] = "c3";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u3:c3"), 1);

  auto session = agent->get_session_by_key("u3:c3");
  ASSERT_NE(session, nullptr);
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 3);
  auto dispatcher_task0 = session->get_task("u3:c3_task_0");
  auto coordinator_task1 = session->get_task("u3:c3_task_1");
  auto generated_task0 = find_task_by_message_id(tasks, "req_task0");
  ASSERT_NE(dispatcher_task0, nullptr);
  ASSERT_NE(coordinator_task1, nullptr);
  ASSERT_NE(generated_task0, nullptr);
  EXPECT_EQ(dispatcher_task0->get_task_type(), "task0");
  EXPECT_EQ(coordinator_task1->get_task_type(), "task1");
  EXPECT_EQ(generated_task0->get_task_type(), "task0");
}

TEST_F(AgentTest, PackageTask0WorkflowMappingIsApplied) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_task0_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "task0-test-agent",
      "version": "1.0.0",
      "workflows": ["workflow_a.json", "workflow_b.json"],
      "coordinator_workflow": "wf_b"
    })";
  }
  {
    std::ofstream wf(tmp / "workflow_a.json");
    wf << R"({"id":"wf_a","name":"wf_a","version":"1.0.0","nodes":[{"id":"n1","name":"n1","type":"TASK"}],"edges":[]})";
  }
  {
    std::ofstream wf(tmp / "workflow_b.json");
    wf << R"({"id":"wf_b","name":"wf_b","version":"1.0.0","nodes":[{"id":"n2","name":"n2","type":"TASK"}],"edges":[]})";
  }

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req_task0_map";
  req.metadata["user_id"] = "u4";
  req.metadata["client_id"] = "c4";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u4:c4"), 1);

  auto session = agent->get_session_by_key("u4:c4");
  ASSERT_NE(session, nullptr);
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 3);
  auto mapped_task = find_task_by_message_id(tasks, "req_task0_map");
  ASSERT_NE(mapped_task, nullptr);
  EXPECT_EQ(mapped_task->get_task_type(), "task0");
  EXPECT_EQ(mapped_task->get_workflow_id(), "wf_b");

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
      "workflows": ["workflow_a.json", "workflow_b.json"],
      "coordinator_workflow": "wf_b"
    })";
  }
  {
    std::ofstream wf(tmp / "workflow_a.json");
    wf << R"({"id":"wf_a","name":"wf_a","version":"1.0.0","nodes":[{"id":"n1","name":"n1","type":"TASK"}],"edges":[]})";
  }
  {
    std::ofstream wf(tmp / "workflow_b.json");
    wf << R"({"id":"wf_b","name":"wf_b","version":"1.0.0","nodes":[{"id":"n2","name":"n2","type":"TASK"}],"edges":[]})";
  }

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  framework::MMAgentMsgQue::Message req;
  req.message_id = "req_workflow_override";
  req.metadata["user_id"] = "u6";
  req.metadata["client_id"] = "c6";
  req.metadata["workflow_id"] = "wf_a";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u6:c6"), 1);

  auto session = agent->get_session_by_key("u6:c6");
  ASSERT_NE(session, nullptr);
  auto task = find_task_by_message_id(session->get_tasks(), "req_workflow_override");
  ASSERT_NE(task, nullptr);
  EXPECT_EQ(task->get_workflow_id(), "wf_a");

  fs::remove_all(tmp);
}

TEST_F(AgentTest, DefaultAndDynamicSessionsContainTask0DispatcherAndTask1Coordinator) {
  auto agent = std::make_shared<framework::MMAgent>(base_config());
  auto default_session = agent->get_default_session();
  ASSERT_NE(default_session, nullptr);
  EXPECT_EQ(default_session->get_session_id(), "0");
  ASSERT_NE(default_session->get_task_coordinator(), nullptr);
  ASSERT_NE(default_session->get_task_dispatcher(), nullptr);
  auto default_task0 = default_session->get_task("0_task_0");
  auto default_task1 = default_session->get_task("0_task_1");
  ASSERT_NE(default_task0, nullptr);
  ASSERT_NE(default_task1, nullptr);
  EXPECT_EQ(default_task0->get_task_type(), "task0");
  EXPECT_EQ(default_task1->get_task_type(), "task1");

  auto user_session = agent->get_or_create_session("u5", "c5");
  ASSERT_NE(user_session, nullptr);
  auto session_task0 = user_session->get_task("u5:c5_task_0");
  auto session_task1 = user_session->get_task("u5:c5_task_1");
  ASSERT_NE(session_task0, nullptr);
  ASSERT_NE(session_task1, nullptr);
  EXPECT_EQ(session_task0->get_task_type(), "task0");
  EXPECT_EQ(session_task1->get_task_type(), "task1");
}

TEST_F(AgentTest, AgentJsonPrecreatesSessionsWithCoordinatorTask0) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_precreate_sessions_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "precreate-session-agent",
      "version": "1.0.0",
      "workflows": ["workflow_main.json"]
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "precreate_session_count": 3
    })";
  }
  {
    std::ofstream wf(tmp / "workflow_main.json");
    wf << R"({"id":"wf_main","name":"wf_main","version":"1.0.0","nodes":[{"id":"n1","name":"n1","type":"TASK"}],"edges":[]})";
  }

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  auto sessions = agent->get_sessions();
  ASSERT_EQ(sessions.size(), 4);

  for(int i = 1; i <= 3; ++i) {
    const std::string session_id = "precreated_session_" + std::to_string(i);
    auto session = agent->get_session_by_key(session_id);
    ASSERT_NE(session, nullptr);
    EXPECT_NE(session->get_task_coordinator(), nullptr);
    auto task0 = session->get_task(session_id + "_task_0");
    auto task1 = session->get_task(session_id + "_task_1");
    ASSERT_NE(task0, nullptr);
    ASSERT_NE(task1, nullptr);
    EXPECT_EQ(task0->get_task_type(), "task0");
    EXPECT_EQ(task1->get_task_type(), "task1");
  }

  fs::remove_all(tmp);
}

}  // namespace tests
}  // namespace agentflow
