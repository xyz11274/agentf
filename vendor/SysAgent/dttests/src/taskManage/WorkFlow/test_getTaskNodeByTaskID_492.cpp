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

class TestGetTaskNodeByTaskID: public ::testing::Test
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

TEST_F(TestGetTaskNodeByTaskID, emptyTaskID) {
    // Given
    WorkFlow workFlow;

    // When
    shared_ptr<TaskNode> result = workFlow.getTaskNodeByTaskID("");

    // Then
    EXPECT_EQ(result, nullptr);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetTaskNodeByTaskID, invalidTaskIDFormat) {
    // Given
    WorkFlow workFlow;

    // When
    shared_ptr<TaskNode> result = workFlow.getTaskNodeByTaskID("invalidTaskID");

    // Then
    EXPECT_EQ(result, nullptr);

    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestGetTaskNodeByTaskID, taskNodeNotFound) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.taskNodes.resize(1);
//    workFlow.taskNodes[0] = make_shared<TaskNode>();
//
//    // When
//    shared_ptr<TaskNode> result = workFlow.getTaskNodeByTaskID("uuid_1_99");
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestGetTaskNodeByTaskID, validTaskID) {
//    // Given
//    WorkFlow workFlow;
//    workFlow.taskNodes.resize(2);
//    workFlow.taskNodes[0] = make_shared<TaskNode>();
//    workFlow.taskNodes[1] = make_shared<TaskNode>();
//    workFlow.taskNodes[1]->childrenNode.resize(1);
//    workFlow.taskNodes[1]->childrenNode[0] = make_shared<TaskNode>();
//
//    // When
//    shared_ptr<TaskNode> result = workFlow.getTaskNodeByTaskID("uuid_1_0");
//
//    // Then
//    EXPECT_EQ(result, workFlow.taskNodes[1]->childrenNode[0]);
//
//    // Clean
//    GlobalMockObject::verify();
//}

