/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include <future>
#include <memory>
#include <utility>
#include "framework_common/utils/env_util.h"
#include "taskManage/Task.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "config/Config.h"
#include "memory/Location.h"
#include "mm_customedreact_llm_component.h"
#include "model/PromptManager.h"
#include "queue/QueueManager.h"
#include "taskManage/QAAgentCallback.h"
#include "taskManage/TeHelper.h"
#include "taskManage/WorkFlow.h"
#include "ruleCognition/CommonDefinition.h"
#include "model/ReactLLMTask.h"

using namespace mockcpp;

class TestATATaskExec : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestATATaskExec, ShouldUpdateRequestContentAndSend_WhenFirstExecutionWithOfficeAgent) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.sender = "QAAgent";
    accessCtrl->header.receiver = "OfficeAgent";
    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;

    Data data("category", "intent", {{"convert", "0"}});
    std::string taskName = "task1";
    std::shared_ptr<Agent> agent = nullptr;
    std::string agentName = "OfficeAgent";
    std::string request = "";
    auto ataTask = std::make_shared<ATATask>(taskName, nullptr, agentName, request);
    ataTask->taskStatus = TaskStatus::PENDING;

    std::string ip = "192.168.1.100";
    MOCKER(&Utils::GetBaseIp).stubs().will(returnValue(ip));
    MOCKER_CPP(&Agent::sendRestfulRequest, bool (Agent::*)(const std::string& message) const).stubs().will(returnValue(true));

    // When
    ataTask->exec(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->taskAssignment.data.userInputObject, "{\"code\":200003,\"data\":{\"intent\":\"\",\"params\":null},\"msg\":\"\",\"retCode\":200}");
    // 校验 accessCtrl->taskAssignment.data.userInputText 和 userInputObject 是否正确设置
    // 校验 agent->sendRestfulRequest 是否被调用
}

TEST_F(TestATATaskExec, ShouldSendToAgent_WhenSenderIsAppOrLtServer) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.sender = "APP";
    accessCtrl->header.receiver = "SomeAgent";
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;

    std::string taskName = "task1";
    std::string agentName = "OfficeAgent";
    std::string request = "";
    auto ataTask = std::make_shared<ATATask>(taskName, nullptr, agentName, request);
    ataTask->taskStatus = TaskStatus::RUNNING;

    std::string ip = "192.168.1.100";
    MOCKER(&Utils::GetBaseIp).stubs().will(returnValue(ip));
    MOCKER_CPP(&Agent::sendRestfulRequest, bool (Agent::*)(const std::string& message) const).stubs().will(returnValue(true));

    // When
    ataTask->exec(accessCtrl);

    // Then
    // 校验 agent->sendRestfulRequest 是否被调用
}

TEST_F(TestATATaskExec, ShouldPostToWriteQueue_WhenReceiverIsAppOrLtServer) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.sender = "SomeAgent";
    accessCtrl->header.receiver = "APP";
    accessCtrl->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    accessCtrl->header.teamEnvironmentID = "team1";

    std::string taskName = "task1";
    std::string agentName = "OfficeAgent";
    std::string request = "";
    auto ataTask = std::make_shared<ATATask>(taskName, nullptr, agentName, request);
    ataTask->taskStatus = TaskStatus::RUNNING;

    // Mock TEManager::getTEById
    std::string teID = "teID";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);
    te->session = nullptr;
    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&))
        .stubs()
        .will(returnValue(te));

    // Mock QueueManager::postWriteQueue
    // MOCKER_CPP(&QueueManager::postWriteQueue, void(QueueManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&, PerSession*))
    //     .with(any(), eq(te->session));

    // When
    ataTask->exec(accessCtrl);

    // Then
    // 校验 postWriteQueue 是否被调用
}

TEST_F(TestATATaskExec, ShouldForwardToCurrentAgent_WhenReceiverEqualsAgentName) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.sender = "AnotherAgent";
    accessCtrl->header.receiver = "SomeAgent";
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;

    std::string taskName = "task1";
    std::string agentName = "OfficeAgent";
    std::string request = "";
    auto ataTask = std::make_shared<ATATask>(taskName, nullptr, agentName, request);
    ataTask->taskStatus = TaskStatus::RUNNING;

    MOCKER_CPP(&Agent::sendRestfulRequest, bool (Agent::*)(const std::string& message) const).stubs().will(returnValue(true));
    // When
    ataTask->exec(accessCtrl);

    // Then
    // 校验 agent->sendRestfulRequest 是否被调用
}

TEST_F(TestATATaskExec, ShouldSendToParentTaskAgent_WhenReceiverNotEqualAgentName) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.sender = "SomeAgent";
    accessCtrl->header.receiver = "DifferentAgent";
    accessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
    accessCtrl->header.teamEnvironmentID = "team1";

    std::string taskName = "task1";
    std::string agentName = "OfficeAgent";
    std::string request = "";
    auto ataTask = std::make_shared<ATATask>(taskName, nullptr, agentName, request);
    ataTask->taskStatus = TaskStatus::RUNNING;
    ataTask->taskID = "1234567_0_1";

    // Mock TEManager::getTEById
    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&))
        .stubs()
        .will(returnValue(static_cast<std::shared_ptr<TeamEnvironment>>(nullptr)));
    ataTask->exec(accessCtrl);

    std::string teID = "team1";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);
    ataTask->exec(accessCtrl);

    // Mock WorkFlow::getTaskNodeByTaskID
//    auto taskNode = std::make_shared<TaskNode>();
//    auto parentATATask = std::make_shared<ATATask>("uuid_1_2", nullptr, "ParentAgent", "requestContent");
//    taskNode->task = parentATATask;
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode>(WorkFlow::*)(std::string))
//        .stubs()
//        .with(eq("uuid_1_2"))
//        .will(returnValue(taskNode));
//
//    // Mock dynamic_pointer_cast<ATATask>
//    MOCKER_CPP(&std::dynamic_pointer_cast<ATATask>, std::shared_ptr<ATATask>(std::*)(const std::shared_ptr<Task>&))
//        .stubs()
//        .with(any())
//        .will(returnValue(parentATATask));

    // When
    ataTask->exec(accessCtrl);

    // Then
    // 校验 parentATATask->agent->sendRestfulRequest 是否被调用
}

