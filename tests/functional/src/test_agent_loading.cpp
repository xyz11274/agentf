#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "agent/agent.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"
#include "workflow/node.h"
#include "workflow/edge.h"
#include "workflow/workflow.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {
namespace functional {

class AgentLoadingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code
    }

    void TearDown() override {
        // Cleanup code
    }
};

TEST_F(AgentLoadingTest, LoadBasicAgentPackage) {
    std::string package_path = "examples/agent/basic_agent";
    
    // Ensure path is correct relative to build dir or source dir
    // We assume test is run from build dir, so path might need adjustment
    if (!std::filesystem::exists(package_path)) {
        // Try up one level
        package_path = "../examples/agent/basic_agent";
    }
    
    // In case we are deep in build directory
    if (!std::filesystem::exists(package_path)) {
        package_path = "../../examples/agent/basic_agent";
    }
    
    if (!std::filesystem::exists(package_path)) {
        // Try absolute path if needed, or fail
        FAIL() << "Package path not found: examples/agent/basic_agent";
    }
    
    auto agent = framework::Agent::create_from_package(package_path);
    ASSERT_NE(agent, nullptr);

    // Basic checks
    EXPECT_EQ(agent->get_name(), "basic-agent");
    EXPECT_EQ(agent->get_config().version, "1.0.0");
    EXPECT_EQ(agent->get_config().description, "Basic agent example for AgentFlow");
    
    // Check if workflows are loaded
    // basic_agent/workflow.json has id "basic-workflow"
    // Wait, let's verify the workflow ID in workflow.json
    auto workflow = agent->get_workflow("basic-workflow");
    if (!workflow) {
        // Maybe ID is different, print loaded IDs
        for (const auto& w : agent->get_workflows()) {
            std::cout << "Loaded workflow ID: " << w->get_id() << std::endl;
        }
    }
    ASSERT_NE(workflow, nullptr);
    EXPECT_EQ(workflow->get_id(), "basic-workflow");
}

TEST_F(AgentLoadingTest, LoadCalculatorAgentPackage) {
    std::string package_path = "examples/agent/calculator_agent";
    if (!std::filesystem::exists(package_path)) {
        package_path = "../examples/agent/calculator_agent";
    }
    if (!std::filesystem::exists(package_path)) {
        package_path = "../../examples/agent/calculator_agent";
    }
    
    ASSERT_TRUE(std::filesystem::exists(package_path)) << "Package path not found: " << package_path;

    auto agent = framework::Agent::create_from_package(package_path);
    ASSERT_NE(agent, nullptr);

    EXPECT_EQ(agent->get_id(), "calculator-agent");
    
    // Check if workflows are loaded
    auto workflow = agent->get_workflow("calculator-workflow");
    ASSERT_NE(workflow, nullptr);
}

TEST_F(AgentLoadingTest, LoadInvalidPackage) {
    std::string package_path = "non_existent_path";
    auto agent = framework::Agent::create_from_package(package_path);
    EXPECT_EQ(agent, nullptr);
}

TEST_F(AgentLoadingTest, TaskToWorkflowScheduling) {
    framework::Agent::Config agent_cfg;
    agent_cfg.id = "agent-schedule";
    agent_cfg.name = "agent-schedule";
    auto agent = std::make_shared<framework::Agent>(agent_cfg);

    framework::Workflow::Config wf_cfg;
    wf_cfg.id = "wf-schedule";
    wf_cfg.name = "wf-schedule";
    auto workflow = std::make_shared<framework::Workflow>(wf_cfg);

    framework::Node::Config node_cfg;
    node_cfg.id = "node-run";
    node_cfg.name = "node-run";
    node_cfg.type = "tool";
    node_cfg.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    node_cfg.cpp_function = [](const DataMap&, DataMap&) { return true; };
    workflow->add_node(std::make_shared<framework::Node>(node_cfg));
    agent->add_workflow(workflow);

    runtime::AgentRuntime runtime;
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
    std::string package_path = "examples/agent/basic_agent";
    if (!std::filesystem::exists(package_path)) {
        package_path = "../examples/agent/basic_agent";
    }
    
    auto agent = framework::Agent::create_from_package(package_path);
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
    
    auto agent = framework::Agent::create_from_package(temp_dir);
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
    
    auto agent = framework::Agent::create_from_package(temp_dir);
    EXPECT_EQ(agent, nullptr);
    
    std::filesystem::remove_all(temp_dir);
}

} // namespace functional
} // namespace agentflow
