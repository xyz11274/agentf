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

class TestCreateSubTask: public ::testing::Test
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

//TEST_F(TestCreateSubTask, createSubTask_WorkFlowFinished) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::FINISHED;
//    TaskEntity taskEntity("abc_1_1_1", "SystemAgent", "requestContent");
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//
//    // When
//    string ret = workFlow.createSubTask(taskEntity, accessCtrl);
//
//    // Then
//    EXPECT_EQ(ret, "");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateSubTask, createSubTask_ParentTaskNotFound) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    TaskEntity taskEntity("abc_1_1_1", "SystemAgent", "requestContent");
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string taskID)).stubs().will(returnValue(nullptr));
//
//    // When
//    string ret = workFlow.createSubTask(taskEntity, accessCtrl);
//
//    // Then
//    EXPECT_EQ(ret, "");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateSubTask, createSubTask_SystemAgent) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    TaskEntity taskEntity("abc_1_1_1", "SystemAgent", "requestContent");
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    shared_ptr<TaskNode> parentTaskNode = make_shared<TaskNode>();
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string taskID)).stubs().will(returnValue(parentTaskNode));
//
//    // When
//    string ret = workFlow.createSubTask(taskEntity, accessCtrl);
//
//    // Then
//    EXPECT_EQ(ret, "abc_1_1_1_0");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateSubTask, createSubTask_RouteAgent) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    TaskEntity taskEntity("abc_1_1_1", "RouteAgent", "requestContent");
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    shared_ptr<TaskNode> parentTaskNode = make_shared<TaskNode>();
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string taskID)).stubs().will(returnValue(parentTaskNode));
//
//    // When
//    string ret = workFlow.createSubTask(taskEntity, accessCtrl);
//
//    // Then
//    EXPECT_EQ(ret, "abc_1_1_1_0");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateSubTask, createSubTask_ATAAgent_AgentNotFound) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    TaskEntity taskEntity("abc_1_1_1", "ATAAgent", "requestContent");
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    shared_ptr<TaskNode> parentTaskNode = make_shared<TaskNode>();
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string taskID)).stubs().will(returnValue(parentTaskNode));
//    MOCKER(static_cast<shared_ptr<Agent>(*)(std::string agentName)>(&getAgentInstance)).stubs().will(returnValue(nullptr));
//
//    // When
//    string ret = workFlow.createSubTask(taskEntity, accessCtrl);
//
//    // Then
//    EXPECT_EQ(ret, "");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateSubTask, createSubTask_ATAAgent_AgentFound) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    TaskEntity taskEntity("abc_1_1_1", "ATAAgent", "requestContent");
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    shared_ptr<TaskNode> parentTaskNode = make_shared<TaskNode>();
//    shared_ptr<Agent> agent = make_shared<Agent>("ATAAgent", "name", "description", "url", "port", "baseUrl");
//    MOCKER_CPP(&WorkFlow::getTaskNodeByTaskID, shared_ptr<TaskNode> (WorkFlow::*)(std::string taskID)).stubs().will(returnValue(parentTaskNode));
//    MOCKER(static_cast<shared_ptr<Agent>(*)(std::string agentName)>(&getAgentInstance)).stubs().will(returnValue(agent));
//
//    // When
//    string ret = workFlow.createSubTask(taskEntity, accessCtrl);
//
//    // Then
//    EXPECT_EQ(ret, "abc_1_1_1_0");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
