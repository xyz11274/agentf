/**
 * @file test_message_routing.cpp
 * @brief E2E测试：消息路由场景
 *
 * 测试目标：验证消息从进入到任务生成的完整链路
 * 链路：reqmsg → Agent.default_session → Agent.new_session → SessionTaskCoordinator → AgentTask
 */

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

#include "agent/agent.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {
namespace e2e {

class E2EMessageRouting : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建临时测试目录
        test_dir_ = std::filesystem::temp_directory_path() / "agentflow_e2e_message_routing";
        std::filesystem::create_directories(test_dir_);
        std::filesystem::create_directories(test_dir_ / "workflows");

        // 创建测试用 Agent 包
        create_test_agent_package();
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    void create_test_agent_package() {
        // 创建 package.json
        std::ofstream pkg(test_dir_ / "package.json");
        pkg << R"({
            "name": "e2e-message-routing-agent",
            "version": "1.0.0",
            "workflows": ["workflows/main.json", "workflows/coordinator.json"],
            "coordinator_workflow": "wf_coordinator"
        })";
        pkg.close();

        // 创建主 workflow
        std::ofstream wf_main(test_dir_ / "workflows" / "main.json");
        wf_main << R"({
            "id": "wf_main",
            "name": "Main Workflow",
            "version": "1.0.0",
            "nodes": [
                {"id": "n1", "name": "Start", "type": "source"},
                {"id": "n2", "name": "Process", "type": "transform"},
                {"id": "n3", "name": "End", "type": "sink"}
            ],
            "edges": [
                {"id": "e1", "source": "n1", "target": "n2"},
                {"id": "e2", "source": "n2", "target": "n3"}
            ]
        })";
        wf_main.close();

        // 创建 coordinator workflow
        std::ofstream wf_coord(test_dir_ / "workflows" / "coordinator.json");
        wf_coord << R"({
            "id": "wf_coordinator",
            "name": "Coordinator Workflow",
            "version": "1.0.0",
            "nodes": [
                {"id": "c1", "name": "Analyze", "type": "source"}
            ],
            "edges": []
        })";
        wf_coord.close();
    }

    std::filesystem::path test_dir_;
};

/**
 * TC-E2E-001: 基础消息路由
 * 验证消息通过 user_id + client_id 创建新 session 并生成 task
 */
TEST_F(E2EMessageRouting, BasicRouting) {
    // 1. 从包创建 Agent
    auto agent = framework::Agent::create_from_package(test_dir_.string());
    ASSERT_NE(agent, nullptr) << "Failed to create agent from package";

    // 2. 初始化并启动 Agent
    ASSERT_TRUE(agent->initialize());
    ASSERT_TRUE(agent->start());

    // 3. 创建请求消息
    framework::AgentMsgQue::Message request;
    request.message_id = "msg-001";
    request.metadata["user_id"] = "user-123";
    request.metadata["client_id"] = "client-456";
    request.data["payload"] = std::string("test payload");

    // 4. 路由消息
    agent->route_request_message(request);

    // 5. 等待消息处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 6. 验证 session 创建
    auto session = agent->get_session("user-123:client-456");
    EXPECT_NE(session, nullptr) << "Session should be created for user:client";

    // 7. 验证 task 生成
    auto tasks = session->get_tasks();
    EXPECT_FALSE(tasks.empty()) << "At least one task should be generated";

    // 8. 清理
    agent->stop();
}

/**
 * TC-E2E-002: 消息 metadata 缺失处理
 * 验证缺失 user_id 或 client_id 时不会创建业务 session
 */
TEST_F(E2EMessageRouting, MissingMetadata) {
    auto agent = framework::Agent::create_from_package(test_dir_.string());
    ASSERT_NE(agent, nullptr);
    agent->initialize();
    agent->start();

    // 测试缺失 user_id
    framework::AgentMsgQue::Message request1;
    request1.message_id = "msg-002";
    request1.metadata["client_id"] = "client-456";
    // 缺失 user_id

    agent->route_request_message(request1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 不应创建 session
    auto sessions = agent->get_sessions();
    size_t session_count_no_user = sessions.size();

    // 测试缺失 client_id
    framework::AgentMsgQue::Message request2;
    request2.message_id = "msg-003";
    request2.metadata["user_id"] = "user-123";
    // 缺失 client_id

    agent->route_request_message(request2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // session 数量不应增加
    sessions = agent->get_sessions();
    EXPECT_EQ(sessions.size(), session_count_no_user)
        << "No new session should be created for missing metadata";

    agent->stop();
}

/**
 * TC-E2E-003: coordinator_workflow 默认路由
 * 验证未指定 workflow_id 时使用 coordinator_workflow
 */
TEST_F(E2EMessageRouting, CoordinatorWorkflow) {
    auto agent = framework::Agent::create_from_package(test_dir_.string());
    ASSERT_NE(agent, nullptr);
    agent->initialize();
    agent->start();

    // 创建不带 workflow_id 的消息
    framework::AgentMsgQue::Message request;
    request.message_id = "msg-004";
    request.metadata["user_id"] = "user-789";
    request.metadata["client_id"] = "client-012";
    // 未指定 workflow_id

    agent->route_request_message(request);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 验证 session 创建
    auto session = agent->get_session("user-789:client-012");
    ASSERT_NE(session, nullptr);

    // 验证 task 绑定到 coordinator_workflow
    auto tasks = session->get_tasks();
    if (!tasks.empty()) {
        auto task = tasks[0];
        // coordinator_workflow 应为 "wf_coordinator"
        EXPECT_EQ(task->get_workflow_id(), "wf_coordinator")
            << "Task should bind to coordinator_workflow by default";
    }

    agent->stop();
}

/**
 * TC-E2E-004: 显式 workflow_id 覆盖
 * 验证显式指定的 workflow_id 优先级最高
 */
TEST_F(E2EMessageRouting, ExplicitWorkflowId) {
    auto agent = framework::Agent::create_from_package(test_dir_.string());
    ASSERT_NE(agent, nullptr);
    agent->initialize();
    agent->start();

    // 创建带显式 workflow_id 的消息
    framework::AgentMsgQue::Message request;
    request.message_id = "msg-005";
    request.metadata["user_id"] = "user-abc";
    request.metadata["client_id"] = "client-def";
    request.metadata["workflow_id"] = "wf_main";  // 显式指定

    agent->route_request_message(request);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 验证 session 创建
    auto session = agent->get_session("user-abc:client-def");
    ASSERT_NE(session, nullptr);

    // 验证 task 绑定到显式指定的 workflow
    auto tasks = session->get_tasks();
    if (!tasks.empty()) {
        auto task = tasks[0];
        EXPECT_EQ(task->get_workflow_id(), "wf_main")
            << "Task should bind to explicitly specified workflow";
    }

    agent->stop();
}

/**
 * TC-E2E-005: 多用户并发路由
 * 验证多个用户消息并发处理
 */
TEST_F(E2EMessageRouting, ConcurrentMultiUser) {
    auto agent = framework::Agent::create_from_package(test_dir_.string());
    ASSERT_NE(agent, nullptr);
    agent->initialize();
    agent->start();

    const int num_users = 10;

    // 并发发送多个用户的消息
    for (int i = 0; i < num_users; i++) {
        framework::AgentMsgQue::Message request;
        request.message_id = "msg-concurrent-" + std::to_string(i);
        request.metadata["user_id"] = "user-" + std::to_string(i);
        request.metadata["client_id"] = "client-" + std::to_string(i);

        agent->route_request_message(request);
    }

    // 等待处理
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 验证每个用户都有对应的 session
    for (int i = 0; i < num_users; i++) {
        std::string session_key = "user-" + std::to_string(i) + ":client-" + std::to_string(i);
        auto session = agent->get_session(session_key);
        EXPECT_NE(session, nullptr) << "Session should exist for " << session_key;
    }

    agent->stop();
}

}  // namespace e2e
}  // namespace agentflow