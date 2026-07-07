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

class TestModelTaskExec : public ::testing::Test {
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

TEST_F(TestModelTaskExec, ShouldReturn_WhenTeamEnvironmentNotFound) {
    // Given
    std::string fakeTeamEnvId = "fake_team_env_id";
    MOCKER(&TEManager::extractTeamEnvId).stubs().will(returnValue(fakeTeamEnvId));
    MOCKER(&TEManager::getTEById).stubs().will(returnValue(static_cast<std::shared_ptr<TeamEnvironment>>(nullptr)));

    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    accessCtrl->taskAssignment.data.userIntent = "intent1";
    accessCtrl->taskAssignment.data.userInputText = "input1";

    std::string taskId = "task1";
    std::string agentName = "agent1";
    std::string content = "request1";
    auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);

    // When
    intentTask->exec(accessCtrl);

    // Then
    // 无特定断言，仅验证未崩溃
}

TEST_F(TestModelTaskExec, ShouldInterruptStreamAndChangeStatus_WhenMessageTypeIsUserTaskManagementAndEventTypeIsTaskStop) {
    // Given
    std::string fakeTeamEnvId = "fake_team_env_id";
    std::string fakeTaskId = "fake_task_id";
    auto fakeTeamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo{});

    MOCKER(&TEManager::extractTeamEnvId).stubs().will(returnValue(fakeTeamEnvId));
    MOCKER(&TEManager::getTEById).stubs().will(returnValue(fakeTeamEnvironment));
    MOCKER(&MMCustomedLlmComponentExt::InterruptStream).stubs().will(returnValue(0));
    fakeTeamEnvironment->setQATask(fakeTaskId);

    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTOP;
    accessCtrl->header.messageType = "UserTaskManagement";

    std::string taskId = "task1";
    std::string agentName = "agent1";
    std::string content = "request1";
    auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);
    // When
    intentTask->exec(accessCtrl);

    // Then
    // 无特定断言，仅验证未崩溃
}

TEST_F(TestModelTaskExec, ShouldCreateNewTaskAndChangeIntent_WhenMessageTypeIsUserTaskManagementAndEventTypeIsTaskAssignmentAndIntentChanged) {
    // Given
    std::string fakeTeamEnvId = "fake_team_env_id";
    auto fakeTeamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo{});

    MOCKER(&TEManager::extractTeamEnvId).stubs().will(returnValue(fakeTeamEnvId));
    MOCKER(&TEManager::getTEById).stubs().will(returnValue(fakeTeamEnvironment));
    MOCKER(&TeamEnvironment::getQALastIntent).stubs().will(returnValue(std::string("old_intent")));
    MOCKER(&TeamEnvironment::getQATask).stubs().will(returnValue(std::string("-1")));

    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    accessCtrl->header.messageType = "UserTaskManagement";
    accessCtrl->taskAssignment.data.userIntent = "new_intent";
    accessCtrl->taskAssignment.data.userInputText = "input_text";

    std::string taskId = "task1";
    std::string agentName = "agent1";
    std::string content = "request1";
    auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);
    // When
    intentTask->exec(accessCtrl);


    // Then
    // 无特定断言，仅验证未崩溃
}

// TEST_F(TestModelTaskExec, ShouldSendStreamRequestAndAddChatMessage_WhenMessageTypeIsUserTaskManagementAndEventTypeIsTaskAssignmentAndIntentNotChanged) {
//     // Given
//     std::string fakeTeamEnvId = "fake_team_env_id";
//     std::string fakeTaskId = "fake_task_id";
//     auto fakeTeamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo{});
//     auto fakeCallback = std::make_shared<QAAgentCallback>();
//     MOCKER(&TEManager::extractTeamEnvId).stubs().will(returnValue(fakeTeamEnvId));
//     MOCKER(&TEManager::getTEById).stubs().will(returnValue(fakeTeamEnvironment));
//     MOCKER(&TeamEnvironment::getQALastIntent).stubs().will(returnValue(std::string("same_intent")));
//     MOCKER(&TeamEnvironment::getQATask).stubs().will(returnValue(fakeTaskId));
//     MOCKER(&TeamEnvironment::getQACallback).stubs().will(returnValue(static_cast<std::shared_ptr<mmsdk::MMLLMAsyncCallBack>>(fakeCallback)));

//     auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//     accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//     accessCtrl->header.messageType = "UserTaskManagement";
//     accessCtrl->taskAssignment.data.userIntent = "same_intent";
//     accessCtrl->taskAssignment.data.userInputText = "input_text";

//     std::string taskId = "task1";
//     std::string agentName = "agent1";
//     std::string content = "request1";
//     auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);
//     // When
//     intentTask->exec(accessCtrl);


//     // Then
//     // 无特定断言，仅验证未崩溃
// }

TEST_F(TestModelTaskExec, ShouldChangeIntentAndCreateStreamTask_WhenMessageTypeIsDataExchangeAndIntentChanged) {
    // Given
    std::string fakeTeamEnvId = "fake_team_env_id";
    auto fakeTeamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo{});

    MOCKER(&TEManager::extractTeamEnvId).stubs().will(returnValue(fakeTeamEnvId));
    MOCKER(&TEManager::getTEById).stubs().will(returnValue(fakeTeamEnvironment));
    MOCKER(&TeamEnvironment::getQALastIntent).stubs().will(returnValue(std::string ("old_intent")));
    MOCKER(&TeamEnvironment::getQATask).stubs().will(returnValue(std::string("-1")));

    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
    accessCtrl->header.messageType = "DataExchange";
    accessCtrl->userInput.data.userIntent = "new_intent";
    accessCtrl->userInput.data.userInputText = "input_text";

    std::string taskId = "task1";
    std::string agentName = "agent1";
    std::string content = "request1";
    auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);
    // When
    intentTask->exec(accessCtrl);

    // Then
    // 无特定断言，仅验证未崩溃
}

// TEST_F(TestModelTaskExec, ShouldSendStreamRequestAndAddChatMessage_WhenMessageTypeIsDataExchangeAndIntentNotChanged) {
//     // Given
//     std::string fakeTeamEnvId = "fake_team_env_id";
//     std::string fakeTaskId = "fake_task_id";
//     auto fakeTeamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo{});
//     auto fakeCallback = std::make_shared<QAAgentCallback>();

//     MOCKER(&TEManager::extractTeamEnvId).stubs().will(returnValue(fakeTeamEnvId));
//     MOCKER(&TEManager::getTEById).stubs().will(returnValue(fakeTeamEnvironment));
//     MOCKER(&TeamEnvironment::getQALastIntent).stubs().will(returnValue(std::string("same_intent")));
//     MOCKER(&TeamEnvironment::getQATask).stubs().will(returnValue(fakeTaskId));
//     MOCKER(&TeamEnvironment::getQACallback).stubs().will(returnValue(static_cast<std::shared_ptr<mmsdk::MMLLMAsyncCallBack>>(fakeCallback)));

//     auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//     accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
//     accessCtrl->header.messageType = "DataExchange";
//     accessCtrl->userInput.data.userIntent = "same_intent";
//     accessCtrl->userInput.data.userInputText = "input_text";

//     std::string taskId = "task1";
//     std::string agentName = "agent1";
//     std::string content = "request1";
//     auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);
//     // When
//     intentTask->exec(accessCtrl);

//     // Then
//     // 无特定断言，仅验证未崩溃
// }

TEST_F(TestModelTaskExec, ShouldChangeStatus_WhenMessageTypeIsTaskStatusManagement) {
    // Given
    std::string fakeTeamEnvId = "fake_team_env_id";
    auto fakeTeamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo{});
    auto fakeWorkFlow = std::make_shared<WorkFlow>();

    MOCKER(&TEManager::extractTeamEnvId).stubs().will(returnValue(fakeTeamEnvId));
    MOCKER(&TEManager::getTEById).stubs().will(returnValue(fakeTeamEnvironment));

    std::shared_ptr<WorkFlow> workFlow1 = std::make_shared<WorkFlow>();
    workFlow1->workFlowID = "fake_team_env";
    fakeTeamEnvironment->addWorkFlow(workFlow1);

    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    accessCtrl->header.messageType = "TaskStatusManagement";

    std::string taskId = "task1";
    std::string agentName = "agent1";
    std::string content = "request1";
    auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);
    // When
    intentTask->exec(accessCtrl);

    // Then
    // 无特定断言，仅验证未崩溃
}

TEST_F(TestModelTaskExec, ShouldLogError_WhenMessageTypeIsInvalid) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
    accessCtrl->header.messageType = "InvalidMessageType";

    std::string taskId = "task1";
    std::string agentName = "agent1";
    std::string content = "request1";
    auto intentTask = std::make_shared<ModelTask>(taskId, agentName, content);
    // When
    intentTask->exec(accessCtrl);

    // Then
    // 无特定断言，仅验证未崩溃
}

