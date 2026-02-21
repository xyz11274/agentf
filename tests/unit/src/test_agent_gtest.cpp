#include <gtest/gtest.h>

#include "agent/agent.h"
#include "agent/msgque.h"
#include "agent/session.h"
#include "agent/task.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace tests {

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
  ASSERT_EQ(tasks.size(), 1);
  EXPECT_EQ(tasks[0]->get_session_id(), "u2:c2");
  EXPECT_EQ(tasks[0]->get_workflow_id(), "wf1");
  EXPECT_EQ(tasks[0]->get_task_type(), "collaboration");
  EXPECT_EQ(tasks[0]->get_collaboration_so_path(), "/tmp/demo_collab.so");
}

}  // namespace tests
}  // namespace agentflow
