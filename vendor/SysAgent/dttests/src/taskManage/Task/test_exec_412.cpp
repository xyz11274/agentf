/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
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
#include <future>
#include <memory>
#include <utility>
#include "ruleCognition/CommonDefinition.h"
#include "model/ReactLLMTask.h"

class TestExec: public ::testing::Test
{
protected:
   static void SetUpTestCase()
   {
   }

   static void TearDownTestCase()
   {
   }

   virtual void SetUp()
   {
   }

   virtual void TearDown()
   {
       GlobalMockObject::verify();
   }
};

//TEST_F(TestExec, UserTaskManagement_TASKSTOP) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "UserTaskManagement";
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTOP;
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//
//    auto teamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo());
//    teamEnvironment->setQATask("qa1");
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&)).stubs().with(smirror("te1")).will(returnValue(teamEnvironment));
//
//    MockerCPP(&mmsdk::MMCustomedLlmComponentExt::GetInstance, mmsdk::MMCustomedLlmComponentExt&(*)()).stubs().will(returnValue(mmsdk::MMCustomedLlmComponentExt::GetInstance()));
//    MockerCPP(&mmsdk::MMCustomedLlmComponentExt::InterruptStream, void (mmsdk::MMCustomedLlmComponentExt::*)(const std::string&, std::function<uint32_t(uint32_t)>)).stubs().will(invoke([](const std::string&, std::function<uint32_t(uint32_t)> callback) {
//        callback(0);
//    }));
//
//    MockerCPP(&TEHelper::newTaskStatusReport, std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::string, int, std::string)).stubs().will(returnValue(std::make_shared<SYSAGENT::AccessCtrl>()));
//
//    MockerCPP(&shared_ptr<WorkFlow>::changeStatus, void (shared_ptr<WorkFlow>::*)(const std::string&, std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(invoke([](shared_ptr<WorkFlow>, const std::string&, std::shared_ptr<SYSAGENT::AccessCtrl>) {}));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, UserTaskManagement_TASKASSIGNMENT_NewIntent) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "UserTaskManagement";
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//    accessCtrl->taskAssignment.data.userIntent = "newIntent";
//    accessCtrl->taskAssignment.data.userInputText = "inputText";
//    accessCtrl->taskAssignment.header.deviceId = "device1";
//
//    auto teamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo());
//    teamEnvironment->setQALastIntent("oldIntent");
//    teamEnvironment->setQATask("-1");
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&)).stubs().with(smirror("te1")).will(returnValue(teamEnvironment));
//
//    Mocker(&changeUserIntent, void (*)(std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(invoke([](std::shared_ptr<SYSAGENT::AccessCtrl>) {}));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, UserTaskManagement_TASKASSIGNMENT_SameIntent) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "UserTaskManagement";
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//    accessCtrl->taskAssignment.data.userIntent = "sameIntent";
//    accessCtrl->taskAssignment.data.userInputText = "inputText";
//    accessCtrl->taskAssignment.header.deviceId = "device1";
//
//    auto teamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo());
//    teamEnvironment->setQALastIntent("sameIntent");
//    teamEnvironment->setQATask("qa1");
//    teamEnvironment->setQACallback(std::make_shared<QAAgentCallback>());
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&)).stubs().with(smirror("te1")).will(returnValue(teamEnvironment));
//
//    Mocker(&sendStreamRequest, void (*)(std::string, std::string, std::string, const std::string&)).stubs().will(invoke([](std::string, std::string, std::string, const std::string&) {}));
//    Mocker(&addUerChatMessage, void (*)(const std::string&, const std::string&, const std::string&)).stubs().will(invoke([](const std::string&, const std::string&, const std::string&) {}));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, DataExchange_NewIntent) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "DataExchange";
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//    accessCtrl->userInput.data.userIntent = "newIntent";
//    accessCtrl->userInput.data.userInputText = "inputText";
//    accessCtrl->userInput.header.deviceId = "device1";
//
//    auto teamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo());
//    teamEnvironment->setQALastIntent("oldIntent");
//    teamEnvironment->setQATask("-1");
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&)).stubs().with(smirror("te1")).will(returnValue(teamEnvironment));
//
//    Mocker(&changeUserIntent, void (*)(std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(invoke([](std::shared_ptr<SYSAGENT::AccessCtrl>) {}));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, DataExchange_SameIntent) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "DataExchange";
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//    accessCtrl->userInput.data.userIntent = "sameIntent";
//    accessCtrl->userInput.data.userInputText = "inputText";
//    accessCtrl->userInput.header.deviceId = "device1";
//
//    auto teamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo());
//    teamEnvironment->setQALastIntent("sameIntent");
//    teamEnvironment->setQATask("qa1");
//    teamEnvironment->setQACallback(std::make_shared<QAAgentCallback>());
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&)).stubs().with(smirror("te1")).will(returnValue(teamEnvironment));
//
//    Mocker(&sendStreamRequest, void (*)(std::string, std::string, std::string, const std::string&)).stubs().will(invoke([](std::string, std::string, std::string, const std::string&) {}));
//    Mocker(&addUerChatMessage, void (*)(const std::string&, const std::string&, const std::string&)).stubs().will(invoke([](const std::string&, const std::string&, const std::string&) {}));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, TaskStatusManagement) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "TaskStatusManagement";
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//
//    auto teamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo());
//    teamEnvironment->setQATask("qa1");
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEHelper::GetOrCreateTE, std::shared_ptr<TeamEnvironment>(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&, PerSession*)).stubs().will(returnValue(teamEnvironment));
//
//    auto workFlow = std::make_shared<WorkFlow>();
//    MockerCPP(&shared_ptr<TeamEnvironment>::getWorkFlow, std::shared_ptr<WorkFlow>(shared_ptr<TeamEnvironment>::*)(const std::string&)).stubs().will(returnValue(workFlow));
//
//    MockerCPP(&shared_ptr<WorkFlow>::changeStatus, void (shared_ptr<WorkFlow>::*)(const std::string&, std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(invoke([](shared_ptr<WorkFlow>, const std::string&, std::shared_ptr<SYSAGENT::AccessCtrl>) {}));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, TaskStatusManagement_WorkFlowNull) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "TaskStatusManagement";
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//
//    auto teamEnvironment = std::make_shared<TeamEnvironment>("te1", UserInfo());
//    teamEnvironment->setQATask("qa1");
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEHelper::GetOrCreateTE, std::shared_ptr<TeamEnvironment>(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&, PerSession*)).stubs().will(returnValue(teamEnvironment));
//
//    MockerCPP(&shared_ptr<TeamEnvironment>::getWorkFlow, std::shared_ptr<WorkFlow>(shared_ptr<TeamEnvironment>::*)(const std::string&)).stubs().will(returnValue(nullptr));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, InvalidMessageType) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "InvalidType";
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExec, TeamEnvironmentNotFound) {
//    // Given
//    ModelTask modelTask("task1", "agent1", "request1");
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.messageType = "UserTaskManagement";
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "te1";
//
//    MockerCPP(&TEManager::getInstance, TEManager&(*)()).stubs().will(returnValue(TEManager::getInstance()));
//    MockerCPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment>(TEManager::*)(const std::string&)).stubs().with(smirror("te1")).will(returnValue(nullptr));
//
//    // When
//    modelTask.exec(accessCtrl);
//
//    // Then
//    EXPECT_EQ(modelTask.getStatus(), TaskStatus::RUNNING);
//
//    // Clean
//    GlobalMockObject::verify();
//}

