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

class TestWorkFlow: public ::testing::Test
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

//TEST_F(TestWorkFlow, ut_workFlowIDGeneration) {
//    // Given
//    // Mock: `WorkFlow::generateRandomWorkFlowID`
//    // set up `MOCKER_CPP` for `std::string WorkFlow::generateRandomWorkFlowID();`, will return `"12345678"`
//    MOCKER_CPP(&WorkFlow::generateRandomWorkFlowID, std::string (WorkFlow::*)()).stubs().will(returnValue("12345678"));
//
//    // When
//    // Initialize `WorkFlow` instance
//    WorkFlow workFlow;
//
//    // Then
//    // Expect: workFlow.workFlowID == "12345678"
//    EXPECT_EQ(workFlow.workFlowID, "12345678");
//    // Expect: workFlow.workFlowStatus == WorkFlowStatus::PENDING
//    EXPECT_EQ(workFlow.workFlowStatus, WorkFlowStatus::PENDING);
//
//    // Clean
//    // Use GlobalMockObject::verify() to clean mock
//    GlobalMockObject::verify();
//}

