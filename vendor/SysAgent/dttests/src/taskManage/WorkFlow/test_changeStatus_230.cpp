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

class TestChangeStatus: public ::testing::Test
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

TEST_F(TestChangeStatus, WorkflowFinished) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::FINISHED;
    std::string taskID = "task1";
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    bool isRefuseAccessCtrl = false;

    // When
    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);


}

TEST_F(TestChangeStatus, NotTaskStatusManagement) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
    std::string taskID = "task1";
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
    bool isRefuseAccessCtrl = false;

    // When
    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);


}

TEST_F(TestChangeStatus, MessageTypeNotTaskStatusReport) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
    std::string taskID = "task1";
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    accessCtrl->taskStatusManagement.data.messageType = "OtherMessageType";
    bool isRefuseAccessCtrl = false;

    // When
    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);


}

TEST_F(TestChangeStatus, TaskNodeNotFound) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
    std::string taskID = "task1";
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
    bool isRefuseAccessCtrl = false;
    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string))
    .stubs()
    .will(returnValue(std::shared_ptr<TaskNode>(nullptr)));

    // When
    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);


    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeStatus, TaskStatusReportNotHandled) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
    std::string taskID = "task1";
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
    accessCtrl->taskStatusManagement.data.taskStatusReport = "OtherStatus";
    bool isRefuseAccessCtrl = false;

    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    auto intentTask = std::make_shared<ATATask>(taskID, nullptr, agentName, requestContent, Data());
    auto taskNode = std::make_shared<TaskNode>(intentTask);
    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));

    // When
    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);


    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestChangeStatus, IndexArraySizeNotTwo) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    std::string taskID = "task1";
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
//    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
//    accessCtrl->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
//    bool isRefuseAccessCtrl = false;
//    std::string agentName = "agentName";
//    std::string requestContent = "requestContent";
//    auto intentTask = std::make_shared<ATATask>(taskID, nullptr, agentName, requestContent, Data());
//    auto taskNode = std::make_shared<TaskNode>(intentTask);
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER_CPP(&WorkFlow::getIndexArray, std::vector<std::string> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(std::vector<std::string>{"uuid", "1", "extra"}));
//
//    // When
//    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestChangeStatus, NextTaskNodeNotFound) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    std::string taskID = "task1_0";
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
//    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
//    accessCtrl->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
//    bool isRefuseAccessCtrl = false;
//    std::string agentName = "agentName";
//    std::string requestContent = "requestContent";
//    auto intentTask = std::make_shared<ATATask>(taskID, nullptr, agentName, requestContent, Data());
//    auto taskNode = std::make_shared<TaskNode>(intentTask);
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER_CPP(&WorkFlow::getIndexArray, std::vector<std::string> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(std::vector<std::string>{"uuid", "0"}));
//
//    // When
//    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestChangeStatus, NextTaskNodeFound) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    std::string taskID = "task1_0";
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
//    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
//    accessCtrl->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
//    bool isRefuseAccessCtrl = false;
//    std::string agentName = "agentName";
//    std::string requestContent = "requestContent";
//    auto intentTask = std::make_shared<ATATask>(taskID, nullptr, agentName, requestContent, Data());
//    auto taskNode = std::make_shared<TaskNode>(intentTask);
//
//    std::string taskIDNext = "task2_0";
//    std::string agentNameNext = "agentName";
//    std::string requestContentNext = "requestContent";
//    auto intentTaskNexet = std::make_shared<ATATask>(taskIDNext, nullptr, agentNameNext, requestContentNext, Data());
//    auto nextTaskNode = std::make_shared<TaskNode>(intentTaskNexet);
//
//    workFlow.taskNodes.push_back(nextTaskNode);
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER_CPP(&WorkFlow::getIndexArray, std::vector<std::string> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(std::vector<std::string>{"uuid", "0"}));
//
//    // When
//    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestChangeStatus, IsRefuseAccessCtrlTrue) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    std::string taskID = "task1_0";
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
//    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
//    accessCtrl->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
//    bool isRefuseAccessCtrl = true;
//    auto taskNode = std::make_shared<TaskNode>();
//    taskNode->task = std::make_shared<Task>();
//    auto nextTaskNode = std::make_shared<TaskNode>();
//    nextTaskNode->task = std::make_shared<Task>();
//    workFlow.taskNodes.push_back(nextTaskNode);
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER_CPP(&WorkFlow::getIndexArray, std::vector<std::string> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(std::vector<std::string>{"uuid", "0"}));
//
//    // When
//    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestChangeStatus, TeamEnvironmentIDEmpty) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    std::string taskID = "task1_0";
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
//    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
//    accessCtrl->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
//    bool isRefuseAccessCtrl = false;
//    auto taskNode = std::make_shared<TaskNode>();
//    taskNode->task = std::make_shared<Task>();
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER_CPP(&WorkFlow::getIndexArray, std::vector<std::string> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(std::vector<std::string>{"uuid", "0"}));
//
//    // When
//    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestChangeStatus, TeamEnvironmentNotFound) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    std::string taskID = "task1_0";
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
//    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
//    accessCtrl->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
//    accessCtrl->header.teamEnvironmentID = "nonexistent";
//    bool isRefuseAccessCtrl = false;
//    auto taskNode = std::make_shared<TaskNode>();
//    taskNode->task = std::make_shared<Task>();
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER_CPP(&WorkFlow::getIndexArray, std::vector<std::string> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(std::vector<std::string>{"uuid", "0"}));
//    MOCKER(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (*)(const std::string&)).stubs().will(returnValue(nullptr));
//
//    // When
//    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestChangeStatus, LastTaskExecuted) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    std::string taskID = "task1_0";
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
//    accessCtrl->taskStatusManagement.data.messageType = "TaskStatusReport";
//    accessCtrl->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
//    accessCtrl->header.teamEnvironmentID = "existent";
//    bool isRefuseAccessCtrl = false;
//    auto taskNode = std::make_shared<TaskNode>();
//    taskNode->task = std::make_shared<Task>();
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, std::shared_ptr<TaskNode> (WorkFlow::*)(std::string)).stubs().will(returnValue(taskNode));
//    MOCKER_CPP(&WorkFlow::getIndexArray, std::vector<std::string> (WorkFlow::*)(const std::string&)).stubs().will(returnValue(std::vector<std::string>{"uuid", "0"}));
//    auto teamEnvironment = std::make_shared<TeamEnvironment>();
//    MOCKER(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (*)(const std::string&)).stubs().will(returnValue(teamEnvironment));
//
//    // When
//    workFlow.changeStatus(taskID, accessCtrl, isRefuseAccessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}

