#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include "agent/agent.h"
#include "agent/agent_actor.h"
#include "agent/package.h"
#include "agent/task.h"
#include "runtime/agent_runtime.h"
#include "runtime/runtime_actor.h"
#include "workflow/node.h"
#include "workflow/edge.h"
#include "workflow/workflow.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {
namespace functional {

class AgentLoadingTest : public ::testing::Test {
protected:
    std::string package_path_;

    void SetUp() override {
        // 动态创建测试 agent 包（测试完成后在 TearDown 中销毁）
        namespace fs = std::filesystem;
        auto tmp = fs::temp_directory_path() / "agentflow_test_agent";
        fs::create_directories(tmp);
        package_path_ = tmp.string();

        // package.json
        std::ofstream pkg(tmp / "package.json");
        pkg << "{"
            << "\"name\":\"GeneralHarnessAgent\","
            << "\"version\":\"3.0.0\","
            << "\"description\":\"Test agent (dynamically created)\","
            << "\"author\":\"AgentFlow\","
            << "\"workflows\":[\"coordinator_workflow.json\"],"
            << "\"metadata\":{\"type\":\"test\",\"category\":\"testing\"}"
            << "}";
        pkg.close();

        // coordinator_workflow.json（最小可用 workflow）
        std::ofstream wf(tmp / "coordinator_workflow.json");
        wf << "{"
            << "\"id\":\"coordinator-workflow\","
            << "\"version\":\"3.0\","
            << "\"name\":\"Coordinator Test\","
            << "\"nodes\":[{"
            << "\"id\":\"test_node\",\"name\":\"Test Node\",\"type\":\"task\","
            << "\"execution_mode\":\"cpp_function\","
            << "\"cpp_function\":\"return true;\""
            << "}]"
            << "}";
        wf.close();
    }

    void TearDown() override {
        // 清理动态创建的测试包
        if (!package_path_.empty()) {
            std::filesystem::remove_all(package_path_);
        }
    }
};

TEST_F(AgentLoadingTest, LoadBasicAgentPackage) {
    ASSERT_TRUE(std::filesystem::exists(package_path_)) << "Package path not found: " << package_path_;

    auto agent = framework::MMAgent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);

    // Basic checks
    EXPECT_EQ(agent->get_name(), "GeneralHarnessAgent");
    EXPECT_EQ(agent->get_config().version, "3.0.0");
    
    // Check if workflows are loaded
    auto workflow = agent->get_workflow("coordinator-workflow");
    if (!workflow) {
        // Maybe ID is different, print loaded IDs
        for (const auto& w : agent->get_workflows()) {
            std::cout << "Loaded workflow ID: " << w->get_id() << std::endl;
        }
    }
    ASSERT_NE(workflow, nullptr);
    EXPECT_EQ(workflow->get_id(), "coordinator-workflow");
}

TEST_F(AgentLoadingTest, LoadCalculatorAgentPackage) {
    ASSERT_TRUE(std::filesystem::exists(package_path_)) << "Package path not found: " << package_path_;

    auto agent = framework::MMAgent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);

    EXPECT_EQ(agent->get_id(), "GeneralHarnessAgent");
    
    // Check harness agent workflows are loaded
    auto workflow = agent->get_workflow("coordinator-workflow");
    ASSERT_NE(workflow, nullptr);
}

TEST_F(AgentLoadingTest, LoadInvalidPackage) {
    std::string package_path = "non_existent_path";
    auto agent = framework::MMAgent::create_from_package(package_path);
    EXPECT_EQ(agent, nullptr);
}

TEST_F(AgentLoadingTest, TaskToWorkflowScheduling) {
    framework::MMAgent::Config agent_cfg;
    agent_cfg.id = "agent-schedule";
    agent_cfg.name = "agent-schedule";
    auto agent = std::make_shared<framework::MMAgent>(agent_cfg);

    framework::MMWorkflow::Config wf_cfg;
    wf_cfg.id = "wf-schedule";
    wf_cfg.name = "wf-schedule";
    auto workflow = std::make_shared<framework::MMWorkflow>(wf_cfg);

    framework::MMNode::Config node_cfg;
    node_cfg.id = "node-run";
    node_cfg.name = "node-run";
    node_cfg.type = "tool";
    node_cfg.execution_mode = framework::MMNode::ExecutionMode::CPP_FUNCTION;
    node_cfg.cpp_function = [](const DataMap&, DataMap&) { return true; };
    workflow->add_node(std::make_shared<framework::MMNode>(node_cfg));
    agent->add_workflow(workflow);

    runtime::MMAgentRuntime runtime;
    ASSERT_TRUE(runtime.register_agent(agent));
    ASSERT_TRUE(runtime.start());

    DataMap inputs;
    inputs["request_id"] = std::string("req-schedule");
    EXPECT_TRUE(runtime.execute_workflow(agent->get_id(), workflow->get_id(), inputs));
    EXPECT_TRUE(runtime.stop());
}

TEST_F(AgentLoadingTest, SkillBusinessStatusIsIndependentFromWorkflowSuccess) {
    framework::MMAgent::Config agent_cfg;
    agent_cfg.id = "agent-skill-status";
    agent_cfg.name = "agent-skill-status";
    auto agent = std::make_shared<framework::MMAgent>(agent_cfg);

    framework::MMWorkflow::Config wf_cfg;
    wf_cfg.id = "wf-skill-status";
    wf_cfg.name = "wf-skill-status";
    auto workflow = std::make_shared<framework::MMWorkflow>(wf_cfg);

    framework::MMNode::Config node_cfg;
    node_cfg.id = "node-skill-status";
    node_cfg.name = "node-skill-status";
    node_cfg.type = "tool";
    node_cfg.execution_mode = framework::MMNode::ExecutionMode::CPP_FUNCTION;
    node_cfg.cpp_function = [](const DataMap& inputs, DataMap& outputs) {
        const auto status_it = inputs.find("reported_status");
        if(status_it == inputs.end()) {
            return false;
        }
        const auto* status = std::get_if<std::string>(&status_it->second);
        if(status == nullptr) {
            return false;
        }
        if(*status == "process_error") {
            return false;
        }
        outputs["status"] = *status;
        outputs["skill_name"] = std::string("test-skill");
        outputs["skill_result"] =
            "{\"skill_name\":\"test-skill\",\"status\":\"" + *status + "\"}";
        outputs["artifacts_json"] = std::string("[]");
        return true;
    };
    workflow->add_node(std::make_shared<framework::MMNode>(node_cfg));
    agent->add_workflow(workflow);

    auto session = agent->get_or_create_session("skill-status-user", "skill-status-client");
    ASSERT_NE(session, nullptr);
    auto source_task = session->get_coordinator_task();
    ASSERT_NE(source_task, nullptr);

    runtime::MMAgentRuntime runtime;
    ASSERT_TRUE(runtime.register_agent(agent));
    ASSERT_TRUE(runtime.start());

    bool completion_received = false;
    framework::MMAgentMsgQue::Message completion;
    source_task->configure_message_handlers(
        [](const framework::MMAgentMsgQue::Message&) {},
        [&](const framework::MMAgentMsgQue::Message& message) {
            completion = message;
            completion_received = true;
        });

    auto execute_and_wait = [&](const std::string& status,
                                const std::string& task_type,
                                const std::string& dialogue_id) {
        completion_received = false;
        completion = framework::MMAgentMsgQue::Message{};

        DataMap inputs;
        inputs["reported_status"] = status;
        if(!runtime.execute_workflow(
               agent->get_id(), workflow->get_id(), inputs, source_task->get_task_id(),
               session->get_session_id(), dialogue_id, "", "", task_type,
               task_type == "skill" ? "skill-runner" : "",
               task_type == "skill" ? "test-skill" : "")) {
            return false;
        }

        for(int attempt = 0; attempt < 200 && !completion_received; ++attempt) {
            source_task->process_pending_messages_once();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return completion_received;
    };

    const std::vector<std::string> failure_statuses = {
        "node_timeout", "interrupted", "llm_error", "tool_call_error",
        "tool_call_loop_detected", "max_rounds_reached",
        "dispatch_manifest_error",
    };
    for(const auto& status : failure_statuses) {
        ASSERT_TRUE(execute_and_wait(status, "skill", "dialogue-" + status));
        EXPECT_EQ(completion.content, "workflow_completed") << status;
        const auto result_it = completion.metadata.find("skill_result");
        ASSERT_NE(result_it, completion.metadata.end()) << status;
        EXPECT_EQ(result_it->second,
                  "{\"skill_name\":\"test-skill\",\"status\":\"" + status + "\"}");
        EXPECT_EQ(completion.metadata.at("skill_status"), status);
        EXPECT_EQ(completion.metadata.at("skill_success"), "false");
    }

    ASSERT_TRUE(execute_and_wait("completed", "skill", "dialogue-completed"));
    EXPECT_EQ(completion.content, "workflow_completed");
    EXPECT_EQ(completion.metadata.at("skill_status"), "completed");
    EXPECT_EQ(completion.metadata.at("skill_success"), "true");

    ASSERT_TRUE(execute_and_wait("process_error", "skill", "dialogue-process-error"));
    EXPECT_EQ(completion.content, "workflow_failed");
    EXPECT_EQ(completion.metadata.at("skill_success"), "false");

    ASSERT_TRUE(execute_and_wait("llm_error", "coordinator", "dialogue-coordinator"));
    EXPECT_EQ(completion.content, "workflow_completed");
    EXPECT_EQ(completion.metadata.count("skill_success"), 0u);

    EXPECT_TRUE(runtime.stop());
}

TEST_F(AgentLoadingTest, ConcurrentSkillCompletionsKeepExecutionScopedOutputs) {
    framework::MMAgent::Config agent_cfg;
    agent_cfg.id = "agent-parallel-skill-output";
    agent_cfg.name = "agent-parallel-skill-output";
    auto agent = std::make_shared<framework::MMAgent>(agent_cfg);

    framework::MMWorkflow::Config wf_cfg;
    wf_cfg.id = "wf-parallel-skill-output";
    wf_cfg.name = "wf-parallel-skill-output";
    auto workflow = std::make_shared<framework::MMWorkflow>(wf_cfg);

    framework::MMNode::Config output_cfg;
    output_cfg.id = "output-node";
    output_cfg.name = "output-node";
    output_cfg.type = "tool";
    output_cfg.execution_mode = framework::MMNode::ExecutionMode::CPP_FUNCTION;
    output_cfg.cpp_function = [](const DataMap& inputs, DataMap& outputs) {
        const auto result_it = inputs.find("reported_result");
        if(result_it == inputs.end()) return false;
        const auto* result = std::get_if<std::string>(&result_it->second);
        if(result == nullptr) return false;
        outputs["skill_result"] = *result;
        outputs["skill_name"] = "business-" + *result;
        outputs["status"] = std::string("completed");
        outputs["artifacts_json"] = std::string("[]");
        return true;
    };
    workflow->add_node(std::make_shared<framework::MMNode>(output_cfg));

    framework::MMNode::Config delay_cfg;
    delay_cfg.id = "delay-node";
    delay_cfg.name = "delay-node";
    delay_cfg.type = "tool";
    delay_cfg.execution_mode = framework::MMNode::ExecutionMode::CPP_FUNCTION;
    delay_cfg.cpp_function = [](const DataMap& inputs, DataMap&) {
        const auto delay_it = inputs.find("delay_ms");
        if(delay_it == inputs.end()) return false;
        const auto* delay_text = std::get_if<std::string>(&delay_it->second);
        if(delay_text == nullptr) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(*delay_text)));
        return true;
    };
    workflow->add_node(std::make_shared<framework::MMNode>(delay_cfg));
    agent->add_workflow(workflow);

    auto session = agent->get_or_create_session("parallel-user", "parallel-client");
    ASSERT_NE(session, nullptr);
    auto source_task = session->get_coordinator_task();
    ASSERT_NE(source_task, nullptr);

    runtime::MMAgentRuntime runtime;
    ASSERT_TRUE(runtime.register_agent(agent));
    ASSERT_TRUE(runtime.start());

    std::vector<framework::MMAgentMsgQue::Message> completions;
    source_task->configure_message_handlers(
        [](const framework::MMAgentMsgQue::Message&) {},
        [&](const framework::MMAgentMsgQue::Message& message) {
            completions.push_back(message);
        });

    const std::string dialogue_id = "dialogue-parallel-output";
    const std::string batch_id = "batch:dialogue-parallel-output:round";
    const std::string exec_a = "dispatch:dialogue-parallel-output:round:1";
    const std::string exec_b = "dispatch:dialogue-parallel-output:round:2";

    DataMap inputs_a;
    inputs_a["reported_result"] = std::string("result-a");
    inputs_a["delay_ms"] = std::string("600");
    inputs_a["exec_skill_key"] = exec_a;
    inputs_a["batch_id"] = batch_id;
    ASSERT_TRUE(runtime.execute_workflow(
        agent->get_id(), workflow->get_id(), inputs_a, source_task->get_task_id(),
        session->get_session_id(), dialogue_id, "", "", "skill",
        "skill-task-a", "skill-runner"));

    const std::string result_key = "/session/" + session->get_session_id() +
                                   "/sys/" + dialogue_id + "/skill_result";
    bool result_a_published = false;
    for(int attempt = 0; attempt < 200 && !result_a_published; ++attempt) {
        DataVariant value;
        if(runtime.get_databus(agent->get_id()).get(result_key, value)) {
            const auto* text = std::get_if<std::string>(&value);
            result_a_published = text != nullptr && *text == "result-a";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(result_a_published);

    DataMap inputs_b;
    inputs_b["reported_result"] = std::string("result-b");
    inputs_b["delay_ms"] = std::string("0");
    inputs_b["exec_skill_key"] = exec_b;
    inputs_b["batch_id"] = batch_id;
    ASSERT_TRUE(runtime.execute_workflow(
        agent->get_id(), workflow->get_id(), inputs_b, source_task->get_task_id(),
        session->get_session_id(), dialogue_id, "", "", "skill",
        "skill-task-b", "skill-runner"));

    for(int attempt = 0; attempt < 400 && completions.size() < 2; ++attempt) {
        source_task->process_pending_messages_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(completions.size(), 2);

    std::map<std::string, framework::MMAgentMsgQue::Message> completion_by_exec;
    for(const auto& completion : completions) {
        const auto exec_it = completion.metadata.find("exec_skill_key");
        ASSERT_NE(exec_it, completion.metadata.end());
        completion_by_exec[exec_it->second] = completion;
    }
    ASSERT_EQ(completion_by_exec.size(), 2);
    ASSERT_TRUE(completion_by_exec.count(exec_a));
    ASSERT_TRUE(completion_by_exec.count(exec_b));

    const auto& completion_a = completion_by_exec.at(exec_a);
    const auto& completion_b = completion_by_exec.at(exec_b);
    EXPECT_EQ(completion_a.content, "workflow_completed");
    EXPECT_EQ(completion_b.content, "workflow_completed");
    EXPECT_EQ(completion_a.metadata.at("skill_result"), "result-a");
    EXPECT_EQ(completion_b.metadata.at("skill_result"), "result-b");
    EXPECT_EQ(completion_a.metadata.at("business_skill_name"), "business-result-a");
    EXPECT_EQ(completion_b.metadata.at("business_skill_name"), "business-result-b");
    EXPECT_EQ(completion_a.metadata.at("skill_task_id"), "skill-task-a");
    EXPECT_EQ(completion_b.metadata.at("skill_task_id"), "skill-task-b");
    EXPECT_EQ(completion_a.metadata.at("batch_id"), batch_id);
    EXPECT_EQ(completion_b.metadata.at("batch_id"), batch_id);
    EXPECT_EQ(completion_a.metadata.at("skill_success"), "true");
    EXPECT_EQ(completion_b.metadata.at("skill_success"), "true");
    EXPECT_FALSE(completion_a.metadata.at("workflow_execution_id").empty());
    EXPECT_FALSE(completion_b.metadata.at("workflow_execution_id").empty());
    EXPECT_NE(completion_a.metadata.at("workflow_execution_id"),
              completion_b.metadata.at("workflow_execution_id"));

    EXPECT_TRUE(runtime.stop());
}

TEST_F(AgentLoadingTest, CyclicWorkflowControlFlow) {
    std::string workflow_path = "publish/workflow/pattern/cycled_workflow.json";
    if(!std::filesystem::exists(workflow_path)) {
        workflow_path = "../publish/workflow/pattern/cycled_workflow.json";
    }
    if(!std::filesystem::exists(workflow_path)) {
        workflow_path = "../../publish/workflow/pattern/cycled_workflow.json";
    }
    ASSERT_TRUE(std::filesystem::exists(workflow_path)) << "workflow path not found";

    auto parser = dsl::WorkflowParserFactory::create();
    auto result = parser->parse_workflow_file(workflow_path);
    ASSERT_TRUE(result.is_valid());
    ASSERT_NE(result.workflow, nullptr);
    EXPECT_TRUE(result.workflow->validate());
}

TEST_F(AgentLoadingTest, AgentLifecycle) {
    auto agent = framework::MMAgent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    
    // Test lifecycle transitions
    EXPECT_EQ(agent->get_state(), AgentState::CREATED);
    
    ASSERT_TRUE(agent->initialize());
    EXPECT_EQ(agent->get_state(), AgentState::INITIALIZED);
    
    ASSERT_TRUE(agent->start());
    EXPECT_EQ(agent->get_state(), AgentState::RUNNING);
    
    ASSERT_TRUE(agent->pause());
    EXPECT_EQ(agent->get_state(), AgentState::PAUSED);
    
    ASSERT_TRUE(agent->resume());
    EXPECT_EQ(agent->get_state(), AgentState::RUNNING);
    
    ASSERT_TRUE(agent->stop());
    EXPECT_EQ(agent->get_state(), AgentState::STOPPED);
}

TEST_F(AgentLoadingTest, LoadPackageMissingJson) {
    // Create a temporary directory with no package.json
    std::string temp_dir = "temp_invalid_pkg";
    std::filesystem::create_directory(temp_dir);
    
    auto agent = framework::MMAgent::create_from_package(temp_dir);
    EXPECT_EQ(agent, nullptr);
    
    std::filesystem::remove(temp_dir);
}

TEST_F(AgentLoadingTest, LoadPackageMalformedJson) {
    // Create a temporary directory with malformed package.json
    std::string temp_dir = "temp_malformed_pkg";
    std::filesystem::create_directory(temp_dir);
    
    std::ofstream ofs(temp_dir + "/package.json");
    ofs << "{ \"name\": \"broken-agent\", \"version\": "; // Incomplete JSON
    ofs.close();
    
    auto agent = framework::MMAgent::create_from_package(temp_dir);
    EXPECT_EQ(agent, nullptr);
    
    std::filesystem::remove_all(temp_dir);
}

#ifdef AGENTFLOW_WITH_RAPIDJSON
TEST_F(AgentLoadingTest, AgentActorAutoLoadsPackagesAndRegistersRuntime) {
    namespace fs = std::filesystem;
    fs::path temp_dir = fs::temp_directory_path() / "agentflow_agent_actor_test";
    fs::create_directories(temp_dir);

    // 使用动态创建的测试包路径
    fs::path pkg_root = fs::path(package_path_).parent_path();
    std::string pkg_name = fs::path(package_path_).filename().string();

    fs::path config_path = temp_dir / "AgentService.config";
    std::ofstream ofs(config_path.string());
    ofs << "{"
        << "\"package_root\":\"" << pkg_root.string() << "\","
        << "\"manifests\":[\"" << pkg_name << "\"],"
        << "\"scan_interval_ms\":100,"
        << "\"heartbeat_interval_ms\":100"
        << "}";
    ofs.close();

    auto runtime = std::make_shared<runtime::MMAgentRuntime>();
    auto runtime_actor = std::make_shared<runtime::MMRuntimeActor>(runtime);
    agent::MMAgentActor actor(config_path.string(), runtime_actor);
    actor.on_start();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto agents = runtime->list_agents();
    actor.on_stop();

    fs::remove_all(temp_dir);
    EXPECT_FALSE(agents.empty());
}
#endif

} // namespace functional
} // namespace agentflow
