#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <type_traits>

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

TEST(RuntimeMasterWorkerTest, WorkerExecutesShellScriptViaEngine) {
  namespace fs = std::filesystem;
  const fs::path script = fs::path("build/test_worker_shell.sh");
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "echo worker-shell-ok\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  runtime::WorkerEngine::ExecutionRequest request;
  request.mode = runtime::WorkerEngine::ExecutionMode::SHELL_SCRIPT;
  request.command = script.string();
  auto response = runtime::WorkerEngine::execute_code(request);
  ASSERT_TRUE(response.success);

  auto it = response.outputs.find("stdout");
  ASSERT_NE(it, response.outputs.end());
  const std::string output = std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return v;
        }
        return "";
      },
      it->second);
  EXPECT_NE(output.find("worker-shell-ok"), std::string::npos);
  fs::remove(script);
}

}  // namespace tests
}  // namespace agentflow
