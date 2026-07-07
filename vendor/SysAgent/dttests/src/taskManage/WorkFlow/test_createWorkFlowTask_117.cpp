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

class TestCreateWorkFlowTask: public ::testing::Test
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

TEST_F(TestCreateWorkFlowTask, EmptyIntentTasks) {
    // Given
    std::vector<IntentTask> intentTasks;
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();

    // When
    WorkFlow workFlow;
    bool ret = workFlow.createWorkFlowTask(intentTasks, accessCtrl);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestCreateWorkFlowTask, ValidIntentTasks) {
    // Given
    std::vector<IntentTask> intentTasks = {
        {"FoodieAgent", "request1", "美团", "path1", Data()},
        {"AnyShoppingAgent", "request2", "淘宝", "path2", Data()}
    };
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.sender = "sender1";
    accessCtrl->header.clientExField = "clientExField1";

    // When
    WorkFlow workFlow;
    bool ret = workFlow.createWorkFlowTask(intentTasks, accessCtrl);

    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(workFlow.sender, "sender1");
    EXPECT_EQ(workFlow.clientExField, "clientExField1");
    EXPECT_EQ(workFlow.taskNodes.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestCreateWorkFlowTask, UnsupportedAgent) {
    // Given
    std::vector<IntentTask> intentTasks = {
        {"UnsupportedAgent", "request1", "app1", "path1", Data()}
    };
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.sender = "sender1";
    accessCtrl->header.clientExField = "clientExField1";

    // When
    WorkFlow workFlow;
    bool ret = workFlow.createWorkFlowTask(intentTasks, accessCtrl);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestCreateWorkFlowTask, GetAgentInstanceNull) {
    // Given
    std::vector<IntentTask> intentTasks = {
            IntentTask("FoodieAgent", "request1", "app1", "path1", Data())
    };
}

