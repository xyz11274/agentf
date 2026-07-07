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

class TestExecTask: public ::testing::Test
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

TEST_F(TestExecTask, WorkflowFinishedByUser) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::FINISHED_BY_USER;
    string taskID = "uuid_1_2";
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();

    // When
    workFlow.execTask(taskID, accessCtrl);


}

TEST_F(TestExecTask, WorkflowFinished) {
    // Given
    WorkFlow workFlow;
    workFlow.workFlowStatus = WorkFlowStatus::FINISHED;
    string taskID = "uuid_1_2";
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();

    // When
    workFlow.execTask(taskID, accessCtrl);


}