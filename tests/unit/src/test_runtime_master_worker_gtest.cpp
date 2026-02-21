#include <gtest/gtest.h>

#include "runtime/agent_runtime.h"

namespace agentflow {
namespace tests {

TEST(RuntimeMasterWorkerTest, DeployerRegisterHeartbeatAndDeployWorkflowInstance) {
  runtime::AgentRuntime runtime;
  runtime.set_role(runtime::AgentRuntime::RuntimeRole::MASTER);

  auto worker = std::make_shared<runtime::WorkerEngine>(
      runtime::WorkerEngine::Config{"worker-a", true});
  worker->set_status(runtime::WorkerEngine::WorkerStatus::RUNNING);
  worker->add_supported_workflow("wf-type-1");

  ASSERT_TRUE(runtime.register_worker_engine(worker));
  ASSERT_TRUE(runtime.heartbeat_worker("worker-a"));

  runtime::WorkflowInstanceInfo info;
  info.workflow_instance_id = "wf-inst-1";
  info.workflow_id = "wf-type-1";
  info.agent_object_id = "agent-object";
  info.agent_instance_id = "agent-a";

  ASSERT_TRUE(runtime.deploy_workflow_instance(info, "worker-a"));
}

TEST(RuntimeMasterWorkerTest, SchedulerPrefersSameAgentInstanceThenSharedInstance) {
  runtime::AgentRuntime runtime;
  runtime.set_role(runtime::AgentRuntime::RuntimeRole::MASTER);

  auto worker_a = std::make_shared<runtime::WorkerEngine>(
      runtime::WorkerEngine::Config{"worker-a", true});
  worker_a->set_status(runtime::WorkerEngine::WorkerStatus::RUNNING);
  worker_a->add_supported_workflow("wf-type-1");
  auto worker_b = std::make_shared<runtime::WorkerEngine>(
      runtime::WorkerEngine::Config{"worker-b", true});
  worker_b->set_status(runtime::WorkerEngine::WorkerStatus::RUNNING);
  worker_b->add_supported_workflow("wf-type-1");

  ASSERT_TRUE(runtime.register_worker_engine(worker_a));
  ASSERT_TRUE(runtime.register_worker_engine(worker_b));

  runtime::WorkflowInstanceInfo own_instance;
  own_instance.workflow_instance_id = "wf-own";
  own_instance.workflow_id = "wf-type-1";
  own_instance.agent_object_id = "agent-object";
  own_instance.agent_instance_id = "agent-a";
  ASSERT_TRUE(runtime.deploy_workflow_instance(own_instance, "worker-a"));

  runtime::WorkflowInstanceInfo shared_instance;
  shared_instance.workflow_instance_id = "wf-shared";
  shared_instance.workflow_id = "wf-type-1";
  shared_instance.agent_object_id = "agent-object";
  shared_instance.agent_instance_id = "agent-b";
  ASSERT_TRUE(runtime.deploy_workflow_instance(shared_instance, "worker-b"));

  runtime::master::TaskScheduleRequest request;
  request.task_id = "task-1";
  request.agent_object_id = "agent-object";
  request.agent_instance_id = "agent-a";
  request.workflow_id = "wf-type-1";

  auto result = runtime.schedule_task(request);
  ASSERT_TRUE(result.scheduled);
  EXPECT_EQ(result.workflow_instance_id, "wf-own");

  ASSERT_TRUE(runtime.destroy_workflow_instance("wf-own"));
  result = runtime.schedule_task(request);
  ASSERT_TRUE(result.scheduled);
  EXPECT_EQ(result.workflow_instance_id, "wf-shared");
}

}  // namespace tests
}  // namespace agentflow
