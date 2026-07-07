/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/WorkFlow.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/common_macro.h"
#include "queue/QueueManager.h"
#include "taskManage/AgentManager.h"
#include "taskManage/Task.h"
#include "taskManage/TaskEntity.h"
#include "taskManage/TaskNode.h"
#include <cstddef>
#include <string>

class TestSendTaskCreateFailedMessage: public ::testing::Test
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

TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_teamEnvIdEmpty) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    string workFlowIDorParentTaskID = "123";
    string taskType = "IntentTask";
    accessCtrl->header.teamEnvironmentID = "";

    // When
    WorkFlow workFlow;
    workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);


    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_teamEnvironmentNotFound) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    string workFlowIDorParentTaskID = "123";
    string taskType = "IntentTask";
    accessCtrl->header.teamEnvironmentID = "456";
    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&))
    .stubs()
    .will(returnValue(std::shared_ptr<TeamEnvironment>(nullptr)));

    // When
    WorkFlow workFlow;
    workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);

    // Clean
    GlobalMockObject::verify();
}

// TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_taskStatuReportMsgNull) {
//     // Given
//     shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//     string workFlowIDorParentTaskID = "123";
//     string taskType = "IntentTask";
//     accessCtrl->header.teamEnvironmentID = "456";

//     std::string teID = "456";
//     const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
//     auto teamEnvironment = std::make_shared<TeamEnvironment>(teID, userInfo);

//     MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&))
//     .stubs().will(returnValue(teamEnvironment));
//     MOCKER(static_cast<shared_ptr<SYSAGENT::AccessCtrl>(*)(shared_ptr<TeamEnvironment>, int, string)>(&TEHelper::newTaskStatusReport))
//     .stubs().will(returnValue(std::shared_ptr<SYSAGENT::AccessCtrl>(nullptr)));

//     // When
//     WorkFlow workFlow;
//     workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);


//     // Clean
//     GlobalMockObject::verify();
// }

TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_IntentTask) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    string workFlowIDorParentTaskID = "123";
    string taskType = "IntentTask";
    accessCtrl->header.teamEnvironmentID = "456";

    std::string teID = "456";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto teamEnvironment = std::make_shared<TeamEnvironment>(teID, userInfo);

    shared_ptr<SYSAGENT::AccessCtrl> taskStatuReportMsg = make_shared<SYSAGENT::AccessCtrl>();
    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(teamEnvironment));
    MOCKER(static_cast<shared_ptr<SYSAGENT::AccessCtrl>(*)(shared_ptr<TeamEnvironment>, int, string)>(&TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskStatuReportMsg));
    MOCKER_CPP(&QueueManager::postWriteQueue, void (QueueManager::*)(const shared_ptr<SYSAGENT::AccessCtrl>&, PerSession*)).stubs().will(returnValue(nullptr));

    // When
    WorkFlow workFlow;
    workFlow.sender = "SystemAgent";
    workFlow.clientExField = "clientExField";
    workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);


    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_SubTask_getWorkFlowFailed) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    string workFlowIDorParentTaskID = "123";
    string taskType = "SubTask";
    accessCtrl->header.teamEnvironmentID = "456";
    std::string teID = "456";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto teamEnvironment = std::make_shared<TeamEnvironment>(teID, userInfo);    shared_ptr<SYSAGENT::AccessCtrl> taskStatuReportMsg = make_shared<SYSAGENT::AccessCtrl>();
    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(teamEnvironment));
    MOCKER(static_cast<shared_ptr<SYSAGENT::AccessCtrl>(*)(shared_ptr<TeamEnvironment>, int, string)>(&TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskStatuReportMsg));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, shared_ptr<WorkFlow> (TeamEnvironment::*)(const string&)).stubs().will(returnValue(0));

    // When
    WorkFlow workFlow;
    workFlow.sender = "SystemAgent";
    workFlow.clientExField = "clientExField";
    workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);


    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_SubTask_getTaskNodeByTaskIDNull) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    string workFlowIDorParentTaskID = "123";
    string taskType = "SubTask";
    accessCtrl->header.teamEnvironmentID = "456";
    std::string teID = "456";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto teamEnvironment = std::make_shared<TeamEnvironment>(teID, userInfo);
    shared_ptr<SYSAGENT::AccessCtrl> taskStatuReportMsg = make_shared<SYSAGENT::AccessCtrl>();
    shared_ptr<WorkFlow> workFlowPtr = make_shared<WorkFlow>();
    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(teamEnvironment));
    MOCKER(static_cast<shared_ptr<SYSAGENT::AccessCtrl>(*)(shared_ptr<TeamEnvironment>, int, string)>(&TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskStatuReportMsg));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, shared_ptr<WorkFlow> (TeamEnvironment::*)(std::string)).stubs().will(returnValue(workFlowPtr));
    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(nullptr));

    // When
    WorkFlow workFlow;
    workFlow.sender = "SystemAgent";
    workFlow.clientExField = "clientExField";
    workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);


    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_SubTask_dynamicCastFailed) {
//    // Given
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    string workFlowIDorParentTaskID = "123";
//    string taskType = "SubTask";
//    accessCtrl->header.teamEnvironmentID = "456";
//    shared_ptr<TeamEnvironment> teamEnvironment = make_shared<TeamEnvironment>();
//    shared_ptr<SYSAGENT::AccessCtrl> taskStatuReportMsg = make_shared<SYSAGENT::AccessCtrl>();
//    shared_ptr<WorkFlow> workFlowPtr = make_shared<WorkFlow>();
//    shared_ptr<TaskNode> taskNode = make_shared<TaskNode>();
//    taskNode->task = make_shared<Task>();
//    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(teamEnvironment));
//    MOCKER(static_cast<shared_ptr<SYSAGENT::AccessCtrl>(*)(shared_ptr<TeamEnvironment>, int, string)>(&TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskStatuReportMsg));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, shared_ptr<WorkFlow> (TeamEnvironment::*)(const string&)).stubs().will(returnValue(workFlowPtr));
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//
//    // When
//    WorkFlow workFlow;
//    workFlow.sender = "SystemAgent";
//    workFlow.clientExField = "clientExField";
//    workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestSendTaskCreateFailedMessage, sendTaskCreateFailedMessage_SubTask_Success) {
//    // Given
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    string workFlowIDorParentTaskID = "123";
//    string taskType = "SubTask";
//    accessCtrl->header.teamEnvironmentID = "456";
//    shared_ptr<TeamEnvironment> teamEnvironment = make_shared<TeamEnvironment>();
//    shared_ptr<SYSAGENT::AccessCtrl> taskStatuReportMsg = make_shared<SYSAGENT::AccessCtrl>();
//    shared_ptr<WorkFlow> workFlowPtr = make_shared<WorkFlow>();
//    shared_ptr<TaskNode> taskNode = make_shared<TaskNode>();
//    shared_ptr<ATATask> ataTask = make_shared<ATATask>();
//    shared_ptr<Agent> agent = make_shared<Agent>();
//    agent->setType("AgentType");
//    ataTask->agent = agent;
//    taskNode->task = ataTask;
//    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(teamEnvironment));
//    MOCKER(static_cast<shared_ptr<SYSAGENT::AccessCtrl>(*)(shared_ptr<TeamEnvironment>, int, string)>(&TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskStatuReportMsg));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, shared_ptr<WorkFlow> (TeamEnvironment::*)(const string&)).stubs().will(returnValue(workFlowPtr));
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER(static_cast<void(*)(const string&)>(&Agent::sendRestfulRequest)).stubs().will(returnValue());
//
//    // When
//    WorkFlow workFlow;
//    workFlow.sender = "SystemAgent";
//    workFlow.clientExField = "clientExField";
//    workFlow.sendTaskCreateFailedMessage(accessCtrl, workFlowIDorParentTaskID, taskType);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
