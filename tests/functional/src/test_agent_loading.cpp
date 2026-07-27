#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include "agent/agent.h"
#include "agent/agent_actor.h"
#include "agent/package.h"
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
