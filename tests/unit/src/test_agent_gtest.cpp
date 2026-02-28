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
std::shared_ptr<framework::AgentTask> find_task_by_type(
    const std::vector<std::shared_ptr<framework::AgentTask>>& tasks, const std::string& task_type) {
  for(const auto& task : tasks) {
    if(task && task->get_task_type() == task_type) {
      return task;
    }
  }
  return nullptr;
}
}  // namespace

class AgentTest : public ::testing::Test {
protected:
  framework::Agent::Config base_config() {
    framework::Agent::Config cfg;
    cfg.id = "agent_test";
    cfg.name = "agent_test";
    return cfg;
  }
};

TEST_F(AgentTest, MsgQuePrioritizesHighMessages) {
  framework::AgentMsgQue::Config qcfg;
  qcfg.max_queue_size = 10;
  framework::AgentMsgQue queue(qcfg);

  framework::AgentMsgQue::Message normal;
  normal.message_id = "normal";
  normal.priority = framework::AgentMsgQue::MessagePriority::NORMAL;
  ASSERT_TRUE(queue.enqueue_message(normal));

  framework::AgentMsgQue::Message high;
  high.message_id = "high";
  high.priority = framework::AgentMsgQue::MessagePriority::HIGH;
  ASSERT_TRUE(queue.enqueue_message(high));

  framework::AgentMsgQue::Message out;
  ASSERT_TRUE(queue.dequeue_message(out));
  EXPECT_EQ(out.message_id, "high");
  ASSERT_TRUE(queue.dequeue_message(out));
  EXPECT_EQ(out.message_id, "normal");
}

TEST_F(AgentTest, MsgQueStoresPayloadAsDataRef) {
  framework::AgentMsgQue::Config qcfg;
  framework::AgentMsgQue queue(qcfg);

  framework::AgentMsgQue::Message msg;
  msg.message_id = "payload-msg";
  msg.content = "raw-payload";
  msg.metadata["user_id"] = "u-data";
  msg.metadata["client_id"] = "c-data";
  ASSERT_TRUE(queue.enqueue_message(msg));

  framework::AgentMsgQue::Message out;
  ASSERT_TRUE(queue.dequeue_message(out));
  EXPECT_TRUE(out.content.empty());
  EXPECT_FALSE(out.data_ref.empty());
}

TEST_F(AgentTest, RouteRequestCreatesSessionByUserAndClient) {
  auto agent = std::make_shared<framework::Agent>(base_config());

  framework::AgentMsgQue::Message req;
  req.message_id = "req1";
  req.metadata["user_id"] = "u1";
  req.metadata["client_id"] = "c1";
  ASSERT_TRUE(agent->route_request_message(req));

  auto session = agent->get_session_by_key("u1:c1");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->get_receive_queue_size(), 1);
}

TEST_F(AgentTest, CoordinatorBuildsSessionOwnedTaskWithWorkflowMapping) {
  auto agent = std::make_shared<framework::Agent>(base_config());

  framework::Workflow::Config wf_cfg;
  wf_cfg.id = "wf1";
  wf_cfg.name = "wf1";
  agent->add_workflow(std::make_shared<framework::Workflow>(wf_cfg));

  framework::AgentMsgQue::Message req;
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
  ASSERT_EQ(tasks.size(), 2);
  auto collaboration_task = find_task_by_type(tasks, "collaboration");
  ASSERT_NE(collaboration_task, nullptr);
  EXPECT_EQ(collaboration_task->get_session_id(), "u2:c2");
  EXPECT_EQ(collaboration_task->get_workflow_id(), "wf1");
  EXPECT_EQ(collaboration_task->get_collaboration_so_path(), "/tmp/demo_collab.so");
  std::string message_content;
  std::string message_data_ref;
  EXPECT_TRUE(collaboration_task->get_parameter("message_content", message_content));
  EXPECT_TRUE(collaboration_task->get_parameter("message_data_ref", message_data_ref));
  EXPECT_EQ(message_content, "task-input");
  EXPECT_FALSE(message_data_ref.empty());
}

TEST_F(AgentTest, CoordinatorDefaultsToTask0Type) {
  auto agent = std::make_shared<framework::Agent>(base_config());
  framework::Workflow::Config wf_cfg;
  wf_cfg.id = "wf_default";
  wf_cfg.name = "wf_default";
  agent->add_workflow(std::make_shared<framework::Workflow>(wf_cfg));

  framework::AgentMsgQue::Message req;
  req.message_id = "req_task0";
  req.metadata["user_id"] = "u3";
  req.metadata["client_id"] = "c3";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u3:c3"), 1);

  auto session = agent->get_session_by_key("u3:c3");
  ASSERT_NE(session, nullptr);
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 2);
  auto coordinator_task0 = session->get_task("u3:c3_task_0");
  auto generated_task0 = session->get_task("u3:c3_task_1");
  ASSERT_NE(coordinator_task0, nullptr);
  ASSERT_NE(generated_task0, nullptr);
  EXPECT_EQ(coordinator_task0->get_task_type(), "task0");
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

  auto agent = framework::Agent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  framework::AgentMsgQue::Message req;
  req.message_id = "req_task0_map";
  req.metadata["user_id"] = "u4";
  req.metadata["client_id"] = "c4";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u4:c4"), 1);

  auto session = agent->get_session_by_key("u4:c4");
  ASSERT_NE(session, nullptr);
  auto tasks = session->get_tasks();
  ASSERT_EQ(tasks.size(), 2);
  auto mapped_task = session->get_task("u4:c4_task_1");
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

  auto agent = framework::Agent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  framework::AgentMsgQue::Message req;
  req.message_id = "req_workflow_override";
  req.metadata["user_id"] = "u6";
  req.metadata["client_id"] = "c6";
  req.metadata["workflow_id"] = "wf_a";
  ASSERT_TRUE(agent->route_request_message(req));
  EXPECT_EQ(agent->process_session_messages("u6:c6"), 1);

  auto session = agent->get_session_by_key("u6:c6");
  ASSERT_NE(session, nullptr);
  auto task = session->get_task("u6:c6_task_1");
  ASSERT_NE(task, nullptr);
  EXPECT_EQ(task->get_workflow_id(), "wf_a");

  fs::remove_all(tmp);
}

TEST_F(AgentTest, DefaultAndDynamicSessionsContainTask0CoordinatorTask) {
  auto agent = std::make_shared<framework::Agent>(base_config());
  auto default_session = agent->get_default_session();
  ASSERT_NE(default_session, nullptr);
  EXPECT_EQ(default_session->get_session_id(), "0");
  auto default_task0 = default_session->get_task("0_task_0");
  ASSERT_NE(default_task0, nullptr);
  EXPECT_EQ(default_task0->get_task_type(), "task0");

  auto user_session = agent->get_or_create_session("u5", "c5");
  ASSERT_NE(user_session, nullptr);
  auto session_task0 = user_session->get_task("u5:c5_task_0");
  ASSERT_NE(session_task0, nullptr);
  EXPECT_EQ(session_task0->get_task_type(), "task0");
}

}  // namespace tests
}  // namespace agentflow
