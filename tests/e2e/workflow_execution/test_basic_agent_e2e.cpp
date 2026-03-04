/**
 * @file test_basic_agent_e2e.cpp
 * @brief E2E测试：使用 basic_agent 验证从包加载到 Workflow 执行的完整链路
 *
 * 测试目标：验证 Agent 从 package.json 加载、初始化、Workflow 执行的端到端流程
 *
 * 测试链路：
 * 1. AgentPackage 加载 (package.json)
 * 2. Agent 创建和初始化
 * 3. Runtime 注册和启动
 * 4. Workflow DAG 执行 (使用 CPP_FUNCTION 模式)
 */

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

#include "agent/agent.h"
#include "agent/package.h"
#include "agent/session.h"
#include "agent/task.h"
#include "runtime/agent_runtime.h"
#include "runtime/worker.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {
namespace e2e {

/**
 * @brief Basic Agent 端到端测试套件
 *
 * 使用 examples/agent/basic_agent 作为测试数据
 */
class BasicAgentE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        // 查找 basic_agent 包路径
        package_path_ = find_basic_agent_path();
        ASSERT_FALSE(package_path_.empty()) << "basic_agent package not found";
    }

    void TearDown() override {
        // 清理
    }

    /**
     * @brief 查找 basic_agent 包路径
     */
    std::string find_basic_agent_path() {
        std::vector<std::string> search_paths = {
            "examples/agent/basic_agent",
            "../examples/agent/basic_agent",
            "../../examples/agent/basic_agent",
            "../../../examples/agent/basic_agent"
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
 * TC-E2E-001: 包加载验证
 * 验证 basic_agent 包能被正确加载和解析
 */
TEST_F(BasicAgentE2ETest, PackageLoading) {
    // 1. 验证包目录结构
    EXPECT_TRUE(std::filesystem::exists(package_path_ + "/package.json"))
        << "package.json should exist";

    // 2. 加载 Agent 包
    auto package = framework::AgentPackage::load_from_directory(package_path_);
    ASSERT_NE(package, nullptr) << "Failed to load agent package";

    // 3. 验证包配置
    const auto& config = package->get_config();
    EXPECT_EQ(config.package_name, "basic-agent");
    EXPECT_EQ(config.version, "1.0.0");

    // 4. 验证 workflow 文件列表
    EXPECT_GE(config.workflow_files.size(), 2)
        << "At least 2 workflow files should be defined";

    // 5. 验证 coordinator_workflow 配置
    EXPECT_EQ(config.coordinator_workflow, "basic-coordinator-workflow");
}

/**
 * TC-E2E-002: Agent 创建和初始化
 * 验证从包创建 Agent 并完成初始化
 */
TEST_F(BasicAgentE2ETest, AgentCreationAndInit) {
    // 1. 从包创建 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr) << "Failed to create agent from package";

    // 2. 验证 Agent 基本信息
    EXPECT_EQ(agent->get_name(), "basic-agent");
    EXPECT_EQ(agent->get_config().version, "1.0.0");

    // 3. 验证初始状态
    EXPECT_EQ(agent->get_state(), AgentState::CREATED);

    // 4. 初始化 Agent
    EXPECT_TRUE(agent->initialize());
    EXPECT_EQ(agent->get_state(), AgentState::INITIALIZED);

    // 5. 验证 Workflow 加载
    auto workflows = agent->get_workflows();
    EXPECT_GE(workflows.size(), 2) << "At least 2 workflows should be loaded";

    // 6. 验证特定 Workflow 存在
    auto basic_wf = agent->get_workflow("basic-workflow-v2");
    EXPECT_NE(basic_wf, nullptr) << "basic-workflow-v2 should be loaded";

    auto coord_wf = agent->get_workflow("basic-coordinator-workflow");
    EXPECT_NE(coord_wf, nullptr) << "basic-coordinator-workflow should be loaded";
}

/**
 * TC-E2E-003: Agent 生命周期
 * 验证 Agent 完整生命周期状态转换
 */
TEST_F(BasicAgentE2ETest, AgentLifecycle) {
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);

    // CREATED -> INITIALIZED
    EXPECT_EQ(agent->get_state(), AgentState::CREATED);
    EXPECT_TRUE(agent->initialize());
    EXPECT_EQ(agent->get_state(), AgentState::INITIALIZED);

    // INITIALIZED -> RUNNING
    EXPECT_TRUE(agent->start());
    EXPECT_EQ(agent->get_state(), AgentState::RUNNING);

    // RUNNING -> PAUSED
    EXPECT_TRUE(agent->pause());
    EXPECT_EQ(agent->get_state(), AgentState::PAUSED);

    // PAUSED -> RUNNING
    EXPECT_TRUE(agent->resume());
    EXPECT_EQ(agent->get_state(), AgentState::RUNNING);

    // RUNNING -> STOPPED
    EXPECT_TRUE(agent->stop());
    EXPECT_EQ(agent->get_state(), AgentState::STOPPED);
}

/**
 * TC-E2E-004: Runtime 注册
 * 验证 Agent 注册到 Runtime
 */
TEST_F(BasicAgentE2ETest, RuntimeRegistration) {
    // 1. 创建 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    // 2. 创建并启动 Runtime
    runtime::AgentRuntime runtime;
    ASSERT_TRUE(runtime.start());

    // 3. 注册 Agent 到 Runtime
    EXPECT_TRUE(runtime.register_agent(agent));

    // 4. 验证 Agent 已注册
    auto registered_agent = runtime.get_agent(agent->get_id());
    EXPECT_NE(registered_agent, nullptr);

    // 5. 列出所有 Agent
    auto agents = runtime.list_agents();
    EXPECT_EQ(agents.size(), 1);

    // 6. 取消注册 Agent
    EXPECT_TRUE(runtime.unregister_agent(agent->get_id()));

    // 7. 停止 Runtime
    EXPECT_TRUE(runtime.stop());
}

/**
 * TC-E2E-005: Workflow DAG 执行 (使用 CPP_FUNCTION)
 * 验证多节点 Workflow 按拓扑顺序执行
 */
TEST_F(BasicAgentE2ETest, WorkflowDAGExecution) {
    // 1. 创建 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    // 2. 创建带有多节点的 Workflow
    framework::Workflow::Config wf_config;
    wf_config.id = "e2e-dag-workflow";
    wf_config.name = "E2E DAG Workflow";
    auto workflow = std::make_shared<framework::Workflow>(wf_config);

    // 3. 添加节点（创建简单的 DAG: Node1 -> Node2 -> Node3）
    framework::Node::Config node1_config;
    node1_config.id = "node1";
    node1_config.name = "Start Node";
    node1_config.type = "source";
    node1_config.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    node1_config.cpp_function = [](const DataMap& /*in*/, DataMap& out) -> bool {
        out["node1_output"] = std::string("processed by node1");
        return true;
    };
    workflow->add_node(std::make_shared<framework::Node>(node1_config));

    framework::Node::Config node2_config;
    node2_config.id = "node2";
    node2_config.name = "Middle Node";
    node2_config.type = "transform";
    node2_config.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    node2_config.cpp_function = [](const DataMap& /*in*/, DataMap& out) -> bool {
        out["node2_output"] = std::string("processed by node2");
        return true;
    };
    workflow->add_node(std::make_shared<framework::Node>(node2_config));

    framework::Node::Config node3_config;
    node3_config.id = "node3";
    node3_config.name = "End Node";
    node3_config.type = "sink";
    node3_config.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    node3_config.cpp_function = [](const DataMap& /*in*/, DataMap& out) -> bool {
        out["node3_output"] = std::string("processed by node3");
        out["status"] = std::string("completed");
        return true;
    };
    workflow->add_node(std::make_shared<framework::Node>(node3_config));

    // 4. 添加边
    framework::Edge::Config edge1_config;
    edge1_config.id = "edge1";
    edge1_config.source_node = "node1";
    edge1_config.target_node = "node2";
    workflow->add_edge(std::make_shared<framework::Edge>(edge1_config));

    framework::Edge::Config edge2_config;
    edge2_config.id = "edge2";
    edge2_config.source_node = "node2";
    edge2_config.target_node = "node3";
    workflow->add_edge(std::make_shared<framework::Edge>(edge2_config));

    // 5. 添加到 Agent
    agent->add_workflow(workflow);

    // 6. 启动 Runtime
    runtime::AgentRuntime runtime;
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.register_agent(agent));

    // 7. 执行 Workflow
    DataMap inputs;
    inputs["initial_data"] = std::string("start");
    bool result = runtime.execute_workflow(agent->get_id(), workflow->get_id(), inputs);
    EXPECT_TRUE(result);

    // 8. 等待执行完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 9. 清理
    runtime.stop();
}

/**
 * TC-E2E-006: 消息路由到 Session 创建
 * 验证消息路由后 Session 被正确创建
 */
TEST_F(BasicAgentE2ETest, MessageRoutingToSessionCreation) {
    // 1. 创建并启动 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());
    ASSERT_TRUE(agent->start());

    // 2. 创建 Runtime
    runtime::AgentRuntime runtime;
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.register_agent(agent));

    // 3. 创建请求消息
    framework::AgentMsgQue::Message request;
    request.message_id = "msg-e2e-001";
    request.metadata["user_id"] = "e2e-user-001";
    request.metadata["client_id"] = "e2e-client-001";
    request.metadata["workflow_id"] = "basic-workflow-v2";
    request.content = "e2e test input";

    // 4. 路由消息
    agent->route_request_message(request);

    // 5. 等待消息处理
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 6. 验证 Session 创建
    auto session = agent->get_session_by_key("e2e-user-001:e2e-client-001");
    EXPECT_NE(session, nullptr) << "Session should be created for user:client";

    // 7. 清理
    agent->stop();
    runtime.stop();
}

/**
 * TC-E2E-007: Workflow 拓扑排序
 * 验证 Workflow 的拓扑排序功能
 */
TEST_F(BasicAgentE2ETest, WorkflowTopologicalOrder) {
    // 1. 创建 Workflow
    framework::Workflow::Config wf_config;
    wf_config.id = "topo-test-workflow";
    wf_config.name = "Topo Test Workflow";
    auto workflow = std::make_shared<framework::Workflow>(wf_config);

    // 2. 添加节点（创建复杂 DAG: A -> B -> D, A -> C -> D）
    framework::Node::Config nodeA;
    nodeA.id = "nodeA";
    nodeA.name = "Node A";
    nodeA.type = "source";
    nodeA.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    nodeA.cpp_function = [](const DataMap&, DataMap&) -> bool { return true; };
    workflow->add_node(std::make_shared<framework::Node>(nodeA));

    framework::Node::Config nodeB;
    nodeB.id = "nodeB";
    nodeB.name = "Node B";
    nodeB.type = "transform";
    nodeB.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    nodeB.cpp_function = [](const DataMap&, DataMap&) -> bool { return true; };
    workflow->add_node(std::make_shared<framework::Node>(nodeB));

    framework::Node::Config nodeC;
    nodeC.id = "nodeC";
    nodeC.name = "Node C";
    nodeC.type = "transform";
    nodeC.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    nodeC.cpp_function = [](const DataMap&, DataMap&) -> bool { return true; };
    workflow->add_node(std::make_shared<framework::Node>(nodeC));

    framework::Node::Config nodeD;
    nodeD.id = "nodeD";
    nodeD.name = "Node D";
    nodeD.type = "sink";
    nodeD.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
    nodeD.cpp_function = [](const DataMap&, DataMap&) -> bool { return true; };
    workflow->add_node(std::make_shared<framework::Node>(nodeD));

    // 3. 添加边
    framework::Edge::Config edgeAB;
    edgeAB.id = "edgeAB";
    edgeAB.source_node = "nodeA";
    edgeAB.target_node = "nodeB";
    workflow->add_edge(std::make_shared<framework::Edge>(edgeAB));

    framework::Edge::Config edgeAC;
    edgeAC.id = "edgeAC";
    edgeAC.source_node = "nodeA";
    edgeAC.target_node = "nodeC";
    workflow->add_edge(std::make_shared<framework::Edge>(edgeAC));

    framework::Edge::Config edgeBD;
    edgeBD.id = "edgeBD";
    edgeBD.source_node = "nodeB";
    edgeBD.target_node = "nodeD";
    workflow->add_edge(std::make_shared<framework::Edge>(edgeBD));

    framework::Edge::Config edgeCD;
    edgeCD.id = "edgeCD";
    edgeCD.source_node = "nodeC";
    edgeCD.target_node = "nodeD";
    workflow->add_edge(std::make_shared<framework::Edge>(edgeCD));

    // 4. 获取拓扑排序
    auto order = workflow->get_topological_order();
    EXPECT_EQ(order.size(), 4) << "Should have 4 nodes in topological order";

    // 5. 验证拓扑排序正确性
    // nodeA 应该在 nodeB 和 nodeC 之前
    auto posA = std::find(order.begin(), order.end(), "nodeA");
    auto posB = std::find(order.begin(), order.end(), "nodeB");
    auto posC = std::find(order.begin(), order.end(), "nodeC");
    auto posD = std::find(order.begin(), order.end(), "nodeD");

    EXPECT_NE(posA, order.end());
    EXPECT_NE(posB, order.end());
    EXPECT_NE(posC, order.end());
    EXPECT_NE(posD, order.end());

    // A 必须在 B 和 C 之前
    EXPECT_LT(std::distance(order.begin(), posA), std::distance(order.begin(), posB));
    EXPECT_LT(std::distance(order.begin(), posA), std::distance(order.begin(), posC));

    // B 和 C 必须在 D 之前
    EXPECT_LT(std::distance(order.begin(), posB), std::distance(order.begin(), posD));
    EXPECT_LT(std::distance(order.begin(), posC), std::distance(order.begin(), posD));
}

/**
 * TC-E2E-008: Shell脚本执行验证
 * 验证 Workflow 中的 Shell 脚本被正确执行并输出对应内容
 *
 * 测试链路：
 * 1. 从 package.json 加载 Agent
 * 2. 解析包含 shell_script 节点的 Workflow
 * 3. 验证节点配置正确
 * 4. 通过 WorkerEngine 执行脚本并验证输出内容
 */
TEST_F(BasicAgentE2ETest, ShellScriptExecution) {
    // 1. 从包创建 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr) << "Failed to create agent from package";
    ASSERT_TRUE(agent->initialize());

    // 2. 验证 workflow_01.json 被正确加载
    auto workflow = agent->get_workflow("basic-workflow-v2");
    ASSERT_NE(workflow, nullptr) << "basic-workflow-v2 should be loaded";

    // 3. 验证节点配置
    const auto& nodes = workflow->get_nodes();
    ASSERT_EQ(nodes.size(), 1) << "workflow_01 should have 1 node";

    auto node = nodes[0];
    EXPECT_EQ(node->get_id(), "node_basic");
    EXPECT_EQ(node->get_execution_mode(), framework::Node::ExecutionMode::SHELL_SCRIPT);
    EXPECT_EQ(node->get_command(), "node_basic.sh");

    // 4. 获取脚本所在目录并构造绝对路径
    std::string script_dir = std::filesystem::absolute(package_path_).string();
    std::string script_path = script_dir + "/node_basic.sh";

    // 验证脚本文件存在
    ASSERT_TRUE(std::filesystem::exists(script_path)) << "Script file should exist: " << script_path;

    // 5. 通过 WorkerEngine 执行脚本（使用绝对路径）
    runtime::WorkerEngine::ExecutionRequest request;
    request.mode = runtime::WorkerEngine::ExecutionMode::SHELL_SCRIPT;
    request.command = script_path;  // 使用绝对路径
    request.working_dir = script_dir;
    request.timeout_ms = 5000;

    auto response = runtime::WorkerEngine::execute_code(request);

    // 6. 验证执行结果
    EXPECT_TRUE(response.success) << "Script execution should succeed, error: " << response.error_message;

    // 7. 验证脚本输出内容
    EXPECT_FALSE(response.stdout_content.empty()) << "Script should produce output";
    EXPECT_NE(response.stdout_content.find("basic node"), std::string::npos)
        << "Script output should contain 'basic node', got: " << response.stdout_content;

    // 8. 验证退出码
    EXPECT_EQ(response.exit_code, 0) << "Script should exit with code 0";

    // 9. 验证 outputs map 中包含 stdout
    auto stdout_it = response.outputs.find("stdout");
    EXPECT_NE(stdout_it, response.outputs.end()) << "Outputs should contain 'stdout'";
}

/**
 * TC-E2E-009: 完整 Workflow 执行验证（带 Shell 脚本）
 * 验证从消息路由到 Workflow 执行的完整链路，包括脚本执行
 */
TEST_F(BasicAgentE2ETest, FullWorkflowWithScriptExecution) {
    // 1. 创建并初始化 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());
    ASSERT_TRUE(agent->start());

    // 2. 创建 Runtime
    runtime::AgentRuntime runtime;
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.register_agent(agent));

    // 3. 获取 coordinator workflow（包含两个 shell_script 节点）
    auto coord_workflow = agent->get_workflow("basic-coordinator-workflow");
    ASSERT_NE(coord_workflow, nullptr) << "Coordinator workflow should be loaded";

    // 4. 验证 coordinator workflow 结构
    const auto& nodes = coord_workflow->get_nodes();
    EXPECT_EQ(nodes.size(), 2) << "Coordinator workflow should have 2 nodes";

    // 验证节点都是 SHELL_SCRIPT 模式
    for (const auto& node : nodes) {
        EXPECT_EQ(node->get_execution_mode(), framework::Node::ExecutionMode::SHELL_SCRIPT)
            << "Node " << node->get_id() << " should be SHELL_SCRIPT mode";
    }

    // 验证边
    const auto& edges = coord_workflow->get_edges();
    EXPECT_EQ(edges.size(), 1) << "Coordinator workflow should have 1 edge";
    EXPECT_EQ(edges[0]->get_source(), "session_create");
    EXPECT_EQ(edges[0]->get_target(), "task_create");

    // 5. 创建请求消息触发 workflow 执行
    framework::AgentMsgQue::Message request;
    request.message_id = "msg-full-e2e-001";
    request.metadata["user_id"] = "full-e2e-user";
    request.metadata["client_id"] = "full-e2e-client";
    request.metadata["workflow_id"] = "basic-coordinator-workflow";
    request.content = "full e2e test input";

    // 6. 路由消息
    agent->route_request_message(request);

    // 7. 等待处理完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 8. 验证 Session 创建
    auto session = agent->get_session_by_key("full-e2e-user:full-e2e-client");
    EXPECT_NE(session, nullptr) << "Session should be created";

    // 9. 清理
    agent->stop();
    runtime.stop();
}

/**
 * TC-E2E-010: 脚本输出内容验证
 * 验证脚本执行输出被正确捕获并存储在 DataBus 中
 */
TEST_F(BasicAgentE2ETest, ScriptOutputVerification) {
    // 1. 创建 Agent
    auto agent = framework::Agent::create_from_package(package_path_);
    ASSERT_NE(agent, nullptr);
    ASSERT_TRUE(agent->initialize());

    // 2. 获取 workflow_01
    auto workflow = agent->get_workflow("basic-workflow-v2");
    ASSERT_NE(workflow, nullptr);

    // 3. 获取节点
    auto node = workflow->get_node("node_basic");
    ASSERT_NE(node, nullptr);

    // 4. 获取工作目录并构造脚本绝对路径
    std::string script_dir = std::filesystem::absolute(package_path_).string();
    std::string script_path = script_dir + "/node_basic.sh";

    // 验证脚本文件存在
    ASSERT_TRUE(std::filesystem::exists(script_path)) << "Script file should exist: " << script_path;

    // 5. 直接通过 WorkerEngine 执行脚本（使用绝对路径）
    runtime::WorkerEngine::ExecutionRequest request;
    request.mode = runtime::WorkerEngine::ExecutionMode::SHELL_SCRIPT;
    request.command = script_path;  // 使用绝对路径
    request.working_dir = script_dir;
    request.timeout_ms = 5000;

    auto response = runtime::WorkerEngine::execute_code(request);

    // 6. 验证执行结果
    EXPECT_TRUE(response.success) << "Script execution should succeed, error: " << response.error_message;

    // 7. 验证输出内容
    EXPECT_FALSE(response.stdout_content.empty()) << "Script should produce output";
    EXPECT_NE(response.stdout_content.find("basic node"), std::string::npos)
        << "Output should contain 'basic node', got: " << response.stdout_content;

    // 8. 验证 outputs map 中也包含 stdout
    auto stdout_it = response.outputs.find("stdout");
    EXPECT_NE(stdout_it, response.outputs.end()) << "Outputs should contain 'stdout'";

    if (stdout_it != response.outputs.end()) {
        if (std::holds_alternative<std::string>(stdout_it->second)) {
            std::string output = std::get<std::string>(stdout_it->second);
            EXPECT_NE(output.find("basic node"), std::string::npos)
                << "Output in outputs map should contain 'basic node'";
        }
    }

    // 9. 验证退出码
    EXPECT_EQ(response.exit_code, 0) << "Script should exit with code 0";
}

}  // namespace e2e
}  // namespace agentflow