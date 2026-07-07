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

//TEST_F(TestExec, transferOrReturnTrue) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.receiver = "AISFDEMO";
//    accessCtrl->header.sender = "GUIAgent";
//    accessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
//    accessCtrl->userOutput.data.userOutPutFlag = 1;
//    MOCKER(static_cast<bool(*)(std::shared_ptr<SYSAGENT::AccessCtrl>)>(&transferOrReturn)).stubs().will(returnValue(true));
//
//    ATATask atatask("taskID", std::make_shared<Agent>(), "agentName", "requestContent");
//
//    // When
//    atatask.exec(accessCtrl);
//
//    // Then
//    // No further actions expected, just return
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestExec, transferOrReturnFalse) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.receiver = "AISFDEMO";
//    accessCtrl->header.sender = "GUIAgent";
//    accessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
//    accessCtrl->userOutput.data.userOutPutFlag = 0;
//    MOCKER(static_cast<bool(*)(std::shared_ptr<SYSAGENT::AccessCtrl>)>(&transferOrReturn)).stubs().will(returnValue(false));
//
//    ATATask atatask("taskID", std::make_shared<Agent>(), "agentName", "requestContent");
//
//    // When
//    atatask.exec(accessCtrl);
//
//    // Then
//    // No further actions expected, just return
//
//    // Clean
//    GlobalMockObject::verify();
//}

TEST_F(TestExec, taskStatusPending) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.receiver = "receiver";
    accessCtrl->header.sender = "sender";
    accessCtrl->header.teamEnvironmentID = "teamEnvironmentID";
    accessCtrl->taskAssignment.data.userInputText = "userInputText";
    accessCtrl->taskAssignment.header.taskID = "taskID";
    accessCtrl->taskAssignment.header.receiver = "agentName";

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskID, nullptr, agentName, requestContent);
    atatask.taskStatus = TaskStatus::PENDING;

    // When
//    atatask.exec(accessCtrl);

    // Then
    // No further actions expected, just return

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExec, taskStatusNotPending) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.receiver = "receiver";
    accessCtrl->header.sender = "sender";
    accessCtrl->header.teamEnvironmentID = "teamEnvironmentID";
    accessCtrl->taskAssignment.data.userInputText = "userInputText";
    accessCtrl->taskAssignment.header.taskID = "taskID";
    accessCtrl->taskAssignment.header.receiver = "agentName";

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskID, nullptr, agentName, requestContent);    atatask.taskStatus = TaskStatus::RUNNING;

    // When
//    atatask.exec(accessCtrl);

    // Then
    // No further actions expected, just return

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExec, senderAPP) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.receiver = "receiver";
    accessCtrl->header.sender = "APP";

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskID, nullptr, agentName, requestContent);
    // When
//    atatask.exec(accessCtrl);

    // Then
    // No further actions expected, just return

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExec, receiverAPP) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.receiver = "APP";
    accessCtrl->header.sender = "sender";
    accessCtrl->header.teamEnvironmentID = "teamEnvironmentID";

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskID, nullptr, agentName, requestContent);
    // When
//    atatask.exec(accessCtrl);

    // Then
    // No further actions expected, just return

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExec, receiverNotAPPAndTeamEnvironmentIDEmpty) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.receiver = "receiver";
    accessCtrl->header.sender = "sender";
    accessCtrl->header.teamEnvironmentID = "";

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskID, nullptr, agentName, requestContent);
    // When
//    atatask.exec(accessCtrl);

    // Then
    // No further actions expected, just return

    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestExec, receiverNotAPPAndTeamEnvironmentIDNotFound) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.receiver = "receiver";
//    accessCtrl->header.sender = "sender";
//    accessCtrl->header.teamEnvironmentID = "teamEnvironmentID";
//
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(nullptr));
//
//    ATATask atatask("taskID", std::make_shared<Agent>(), "agentName", "requestContent");
//
//    // When
//    atatask.exec(accessCtrl);
//
//    // Then
//    // No further actions expected, just return
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestExec, receiverEqualsAgentName) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.receiver = "agentName";
//    accessCtrl->header.sender = "sender";
//
//    std::string taskID = "taskID";
//    std::string agentName = "agentName";
//    std::string requestContent = "requestContent";
//    ATATask atatask(taskID, nullptr, agentName, requestContent);
//    // When
//    atatask.exec(accessCtrl);
//
//    // Then
//    // No further actions expected, just return
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestExec, receiverNotEqualsAgentNameAndParentTaskNotFound) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.receiver = "receiver";
//    accessCtrl->header.sender = "sender";
//    accessCtrl->header.teamEnvironmentID = "teamEnvironmentID";
//    accessCtrl->taskAssignment.header.taskID = "taskID_1_2";
//
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>()));
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(nullptr));
//
//    ATATask atatask("taskID_1_2", std::make_shared<Agent>(), "agentName", "requestContent");
//
//    // When
//    atatask.exec(accessCtrl);
//
//    // Then
//    // No further actions expected, just return
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestExec, receiverNotEqualsAgentNameAndParentTaskFound) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.receiver = "receiver";
//    accessCtrl->header.sender = "sender";
//    accessCtrl->header.teamEnvironmentID = "teamEnvironmentID";
//    accessCtrl->taskAssignment.header.taskID = "taskID_1_2";
//
//    std::shared_ptr<TeamEnvironment> teamEnvironment = std::make_shared<TeamEnvironment>();
//    std::shared_ptr<WorkFlow> workFlow = std::make_shared<WorkFlow>();
//    std::shared_ptr<TaskNode> taskNode = std::make_shared<TaskNode>();
//    std::shared_ptr<ATATask> ataTask = std::make_shared<ATATask>("taskID_1", std::make_shared<Agent>(), "agentName", "requestContent");
//
//    taskNode->task = ataTask;
//    workFlow->setTaskNodeByTaskID("taskID_1", taskNode);
//    teamEnvironment->setWorkFlow("taskID_1_2", workFlow);
//
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(teamEnvironment));
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(taskNode));
//
//    ATATask atatask("taskID_1_2", std::make_shared<Agent>(), "agentName", "requestContent");
//
//    // When
//    atatask.exec(accessCtrl);
//
//    // Then
//    // No further actions expected, just return
//
//    // Clean
//    GlobalMockObject::verify();
//}

