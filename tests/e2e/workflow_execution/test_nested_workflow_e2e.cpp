/**
 * @file test_nested_workflow_e2e.cpp
 * @brief E2E测试：验证嵌套 workflow agent 的正确加载和解析
 */

#include <gtest/gtest.h>
#include <filesystem>

#include "agent/agent.h"
#include "agent/package.h"
#include "runtime/worker.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {
namespace e2e {

class NestedWorkflowE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        package_path_ = find_nested_agent_path();
        ASSERT_FALSE(package_path_.empty()) << "nested_workflow_agent package not found";
    }

    std::string find_nested_agent_path() {
        std::vector<std::string> search_paths = {
            "examples/agent/nested_workflow_agent",
            "../examples/agent/nested_workflow_agent",
            "../../examples/agent/nested_workflow_agent",
            "../../../examples/agent/nested_workflow_agent"
        };

        for (const auto& path : search_paths) {
            if (std::filesystem::exists(path + "/package.json")) {
                return path;
            }
        }
        return "";
    }

    std::string package_path_;
};

/**
 * TC-E2E-011: 嵌套 Workflow Agent 包加载
 */
TEST_F(NestedWorkflowE2ETest, PackageLoading) {
    EXPECT_TRUE(std::filesystem::exists(package_path_ + "/package.json"));

    auto package = framework::AgentPackage::load_from_directory(package_path_);
    ASSERT_NE(package, nullptr);

    const auto& config = package->get_config();
    EXPECT_EQ(config.package_name, "nested-workflow-agent");
    EXPECT_EQ(config.version, "1.0.0");
    EXPECT_EQ(config.workflow_files.size(), 4);
}

/**
 * TC-E2E-012: 嵌套 Workflow Agent 创建和初始化
 */
TEST_F(NestedWorkflowE2ETest, AgentCreationAndInit) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);

    EXPECT_TRUE(agent->initialize());

    auto workflows = agent->get_workflows();
    EXPECT_EQ(workflows.size(), 4) << "Should load 4 workflows";
}

/**
 * TC-E2E-013: Main Workflow 结构验证
 */
TEST_F(NestedWorkflowE2ETest, MainWorkflowStructure) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    auto main_wf = agent->get_workflow("main-workflow");
    ASSERT_NE(main_wf, nullptr) << "main-workflow should be loaded";

    const auto& nodes = main_wf->get_nodes();
    EXPECT_EQ(nodes.size(), 4) << "Main workflow should have 4 nodes";

    // 验证 workflow 类型节点
    int workflow_node_count = 0;
    for (const auto& node : nodes) {
        if (node->get_type() == "workflow") {
            workflow_node_count++;
            EXPECT_FALSE(node->get_config().workflow_ref.empty())
                << "workflow node should have workflow_ref";
        }
    }
    EXPECT_EQ(workflow_node_count, 2) << "Should have 2 workflow-type nodes";
}

/**
 * TC-E2E-014: Data Pipeline Workflow 嵌套结构
 */
TEST_F(NestedWorkflowE2ETest, DataPipelineNestedStructure) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    auto data_wf = agent->get_workflow("data-pipeline-workflow");
    ASSERT_NE(data_wf, nullptr) << "data-pipeline-workflow should be loaded";

    const auto& nodes = data_wf->get_nodes();
    EXPECT_EQ(nodes.size(), 3) << "Data pipeline should have 3 nodes";

    // 验证存在嵌套的 validation_workflow 引用
    bool has_nested_wf = false;
    for (const auto& node : nodes) {
        if (node->get_type() == "workflow") {
            has_nested_wf = true;
            EXPECT_EQ(node->get_config().workflow_ref, "validation_workflow.json")
                << "Should reference validation_workflow.json";
        }
    }
    EXPECT_TRUE(has_nested_wf) << "Data pipeline should have nested workflow node";
}

/**
 * TC-E2E-015: Validation Workflow 结构（最深层嵌套）
 */
TEST_F(NestedWorkflowE2ETest, ValidationWorkflowStructure) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    auto val_wf = agent->get_workflow("validation-workflow");
    ASSERT_NE(val_wf, nullptr) << "validation-workflow should be loaded";

    const auto& nodes = val_wf->get_nodes();
    EXPECT_EQ(nodes.size(), 3) << "Validation workflow should have 3 nodes";

    // 验证所有节点都是 shell_script 类型（叶子节点）
    for (const auto& node : nodes) {
        EXPECT_EQ(node->get_execution_mode(), framework::Node::ExecutionMode::SHELL_SCRIPT)
            << "Leaf nodes should use shell_script execution";
    }
}

/**
 * TC-E2E-016: Processing Workflow 结构
 */
TEST_F(NestedWorkflowE2ETest, ProcessingWorkflowStructure) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    auto proc_wf = agent->get_workflow("processing-workflow");
    ASSERT_NE(proc_wf, nullptr) << "processing-workflow should be loaded";

    const auto& nodes = proc_wf->get_nodes();
    EXPECT_EQ(nodes.size(), 3) << "Processing workflow should have 3 nodes";
}

/**
 * TC-E2E-017: 嵌套层级验证
 */
TEST_F(NestedWorkflowE2ETest, NestedLevelsVerification) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    // Level 1: Main workflow - 包含 2 个 workflow_ref 节点
    auto main_wf = agent->get_workflow("main-workflow");
    ASSERT_NE(main_wf, nullptr);

    std::vector<std::string> level1_refs;
    for (const auto& node : main_wf->get_nodes()) {
        if (node->get_type() == "workflow") {
            level1_refs.push_back(node->get_config().workflow_ref);
        }
    }
    EXPECT_EQ(level1_refs.size(), 2);

    // Level 2: Data pipeline - 包含 1 个 workflow_ref 节点
    auto data_wf = agent->get_workflow("data-pipeline-workflow");
    ASSERT_NE(data_wf, nullptr);

    int level2_wf_nodes = 0;
    for (const auto& node : data_wf->get_nodes()) {
        if (node->get_type() == "workflow") {
            level2_wf_nodes++;
        }
    }
    EXPECT_EQ(level2_wf_nodes, 1) << "Data pipeline has 1 nested workflow reference";

    // Level 3: Validation workflow - 没有嵌套，纯执行节点
    auto val_wf = agent->get_workflow("validation-workflow");
    ASSERT_NE(val_wf, nullptr);

    int level3_wf_nodes = 0;
    for (const auto& node : val_wf->get_nodes()) {
        if (node->get_type() == "workflow") {
            level3_wf_nodes++;
        }
    }
    EXPECT_EQ(level3_wf_nodes, 0) << "Validation workflow is the deepest level (no nesting)";
}

/**
 * TC-E2E-018: 嵌套 Workflow 执行验证
 * 验证 main workflow 的节点执行顺序和输出
 */
TEST_F(NestedWorkflowE2ETest, MainWorkflowExecution) {
    // 1. 创建并初始化 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    // 2. 获取 main workflow
    auto main_wf = agent->get_workflow("main-workflow");
    ASSERT_NE(main_wf, nullptr);

    // 3. 验证拓扑排序
    auto topo_order = main_wf->get_topological_order();
    EXPECT_EQ(topo_order.size(), 4);

    // 验证执行顺序: start -> data_pipeline -> final_processing -> end
    auto pos_start = std::find(topo_order.begin(), topo_order.end(), "start");
    auto pos_data = std::find(topo_order.begin(), topo_order.end(), "data_pipeline");
    auto pos_proc = std::find(topo_order.begin(), topo_order.end(), "final_processing");
    auto pos_end = std::find(topo_order.begin(), topo_order.end(), "end");

    ASSERT_NE(pos_start, topo_order.end());
    ASSERT_NE(pos_data, topo_order.end());
    ASSERT_NE(pos_proc, topo_order.end());
    ASSERT_NE(pos_end, topo_order.end());

    // 验证顺序正确
    EXPECT_LT(std::distance(topo_order.begin(), pos_start),
              std::distance(topo_order.begin(), pos_data));
    EXPECT_LT(std::distance(topo_order.begin(), pos_data),
              std::distance(topo_order.begin(), pos_proc));
    EXPECT_LT(std::distance(topo_order.begin(), pos_proc),
              std::distance(topo_order.begin(), pos_end));
}

/**
 * TC-E2E-019: 子 Workflow 执行验证 (Data Pipeline)
 * 验证 data_pipeline workflow 的节点执行顺序
 */
TEST_F(NestedWorkflowE2ETest, DataPipelineExecution) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    auto data_wf = agent->get_workflow("data-pipeline-workflow");
    ASSERT_NE(data_wf, nullptr);

    // 验证拓扑排序: fetch_data -> validate_data -> transform_data
    auto topo_order = data_wf->get_topological_order();
    EXPECT_EQ(topo_order.size(), 3);

    auto pos_fetch = std::find(topo_order.begin(), topo_order.end(), "fetch_data");
    auto pos_validate = std::find(topo_order.begin(), topo_order.end(), "validate_data");
    auto pos_transform = std::find(topo_order.begin(), topo_order.end(), "transform_data");

    ASSERT_NE(pos_fetch, topo_order.end());
    ASSERT_NE(pos_validate, topo_order.end());
    ASSERT_NE(pos_transform, topo_order.end());

    // fetch_data 必须在 validate_data 之前
    EXPECT_LT(std::distance(topo_order.begin(), pos_fetch),
              std::distance(topo_order.begin(), pos_validate));
    // validate_data 必须在 transform_data 之前
    EXPECT_LT(std::distance(topo_order.begin(), pos_validate),
              std::distance(topo_order.begin(), pos_transform));
}

/**
 * TC-E2E-020: Validation Workflow 执行验证（叶子层）
 * 验证最深层 workflow 的节点执行顺序
 */
TEST_F(NestedWorkflowE2ETest, ValidationWorkflowExecution) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    auto val_wf = agent->get_workflow("validation-workflow");
    ASSERT_NE(val_wf, nullptr);

    // 验证拓扑排序: check_schema -> validate_fields -> validation_report
    auto topo_order = val_wf->get_topological_order();
    EXPECT_EQ(topo_order.size(), 3);

    auto pos_schema = std::find(topo_order.begin(), topo_order.end(), "check_schema");
    auto pos_fields = std::find(topo_order.begin(), topo_order.end(), "validate_fields");
    auto pos_report = std::find(topo_order.begin(), topo_order.end(), "validation_report");

    ASSERT_NE(pos_schema, topo_order.end());
    ASSERT_NE(pos_fields, topo_order.end());
    ASSERT_NE(pos_report, topo_order.end());

    // 顺序验证
    EXPECT_LT(std::distance(topo_order.begin(), pos_schema),
              std::distance(topo_order.begin(), pos_fields));
    EXPECT_LT(std::distance(topo_order.begin(), pos_fields),
              std::distance(topo_order.begin(), pos_report));
}

/**
 * TC-E2E-021: Processing Workflow 执行验证
 * 验证 processing workflow 的节点执行顺序
 */
TEST_F(NestedWorkflowE2ETest, ProcessingWorkflowExecution) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    auto proc_wf = agent->get_workflow("processing-workflow");
    ASSERT_NE(proc_wf, nullptr);

    // 验证拓扑排序: aggregate_data -> generate_output -> persist_results
    auto topo_order = proc_wf->get_topological_order();
    EXPECT_EQ(topo_order.size(), 3);

    auto pos_agg = std::find(topo_order.begin(), topo_order.end(), "aggregate_data");
    auto pos_gen = std::find(topo_order.begin(), topo_order.end(), "generate_output");
    auto pos_persist = std::find(topo_order.begin(), topo_order.end(), "persist_results");

    ASSERT_NE(pos_agg, topo_order.end());
    ASSERT_NE(pos_gen, topo_order.end());
    ASSERT_NE(pos_persist, topo_order.end());

    // 顺序验证
    EXPECT_LT(std::distance(topo_order.begin(), pos_agg),
              std::distance(topo_order.begin(), pos_gen));
    EXPECT_LT(std::distance(topo_order.begin(), pos_gen),
              std::distance(topo_order.begin(), pos_persist));
}

/**
 * TC-E2E-022: Shell Script 节点执行验证
 * 验证叶子节点的 shell_script 可以正确执行并产生输出
 */
TEST_F(NestedWorkflowE2ETest, ShellScriptNodeExecution) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    // 获取 validation workflow（最深层，全为 shell_script）
    auto val_wf = agent->get_workflow("validation-workflow");
    ASSERT_NE(val_wf, nullptr);

    // 执行 check_schema 节点
    auto node = val_wf->get_node("check_schema");
    ASSERT_NE(node, nullptr);

    // 验证节点配置
    EXPECT_EQ(node->get_execution_mode(), framework::Node::ExecutionMode::SHELL_SCRIPT);
    EXPECT_FALSE(node->get_command().empty());

    // 获取脚本绝对路径
    std::string script_dir = std::filesystem::absolute(package_path_).string();
    std::string script_path = script_dir + "/check_schema.sh";

    // 验证脚本文件存在
    ASSERT_TRUE(std::filesystem::exists(script_path)) << "Script should exist: " << script_path;

    // 通过 WorkerEngine 执行脚本
    runtime::WorkerEngine::ExecutionRequest request;
    request.mode = runtime::WorkerEngine::ExecutionMode::SHELL_SCRIPT;
    request.command = script_path;
    request.working_dir = script_dir;
    request.timeout_ms = 5000;

    auto response = runtime::WorkerEngine::execute_code(request);

    // 验证执行结果
    EXPECT_TRUE(response.success) << "Script execution should succeed, error: " << response.error_message;

    // 验证输出内容
    EXPECT_FALSE(response.stdout_content.empty()) << "Script should produce output";
    EXPECT_NE(response.stdout_content.find("Checking data schema"), std::string::npos)
        << "Output should contain expected message, got: " << response.stdout_content;

    // 验证退出码
    EXPECT_EQ(response.exit_code, 0) << "Script should exit with code 0";
}

/**
 * TC-E2E-023: 完整嵌套 Workflow 执行链路验证
 * 验证从 main workflow 到叶子节点的完整执行链路
 */
TEST_F(NestedWorkflowE2ETest, FullNestedExecutionChain) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    // 验证完整执行链路
    // Level 1: main-workflow (4 nodes)
    //   └── data_pipeline (Level 2, 3 nodes)
    //         └── validate_data (Level 3, 3 nodes)
    //   └── final_processing (Level 2, 3 nodes)

    // 统计所有 workflow 的总节点数
    int total_nodes = 0;
    auto workflows = agent->get_workflows();
    for (const auto& wf : workflows) {
        total_nodes += static_cast<int>(wf->get_nodes().size());
    }

    // 4 workflows, total 13 nodes
    EXPECT_EQ(total_nodes, 13) << "Total nodes across all workflows should be 13";

    // 验证 workflow 节点类型分布
    int workflow_type_count = 0;
    int shell_script_count = 0;
    int start_end_count = 0;

    for (const auto& wf : workflows) {
        for (const auto& node : wf->get_nodes()) {
            if (node->get_type() == "workflow") {
                workflow_type_count++;
            } else if (node->get_execution_mode() == framework::Node::ExecutionMode::SHELL_SCRIPT) {
                shell_script_count++;
            } else if (node->get_type() == "start" || node->get_type() == "end") {
                start_end_count++;
            }
        }
    }

    // 3 个 workflow 类型节点 (main: 2, data_pipeline: 1)
    EXPECT_EQ(workflow_type_count, 3);
    // 8 个 shell_script 叶子节点
    EXPECT_EQ(shell_script_count, 8);
    // 2 个 start/end 节点
    EXPECT_EQ(start_end_count, 2);
}

}  // namespace e2e
}  // namespace agentflow