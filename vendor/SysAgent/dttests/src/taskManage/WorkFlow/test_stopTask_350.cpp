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

class TestStopTask: public ::testing::Test
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

TEST_F(TestStopTask, WorkflowFinishedByUser) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::FINISHED_BY_USER;
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();

    // When
    workFlow.stopTask(accessCtrl);

    // Then
    EXPECT_EQ(workFlow.workFlowStatus, WorkFlowStatus::FINISHED_BY_USER);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestStopTask, WorkflowFinished) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::FINISHED;
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();

    // When
    workFlow.stopTask(accessCtrl);

    // Then
    EXPECT_EQ(workFlow.workFlowStatus, WorkFlowStatus::FINISHED);

    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestStopTask, WorkflowRunning) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.workFlowStatus = WorkFlowStatus::RUNNING;
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    shared_ptr<TaskNode> taskNode1 = make_shared<TaskNode>();
//    shared_ptr<TaskNode> taskNode2 = make_shared<TaskNode>();
//    workFlow.taskNodes.push_back(taskNode1);
//    workFlow.taskNodes.push_back(taskNode2);
//
//    MOCKER_CPP(&stop, void (shared_ptr<TaskNode>, shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(returnValue());
//
//    // When
//    workFlow.stopTask(accessCtrl);
//
//    // Then
//    EXPECT_EQ(workFlow.workFlowStatus, WorkFlowStatus::FINISHED_BY_USER);
//
//    // Clean
//    GlobalMockObject::verify();
//}

