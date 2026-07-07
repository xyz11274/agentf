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

class TestChangeReceiver: public ::testing::Test
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

TEST_F(TestChangeReceiver, TaskAssignment) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender1";
    workFlow.clientExField = "clientExField1";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    accessCtrl->taskAssignment.header.receiver = "oldReceiver";
    accessCtrl->taskAssignment.header.clientExField = "oldClientExField";

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->taskAssignment.header.receiver, "sender1");
    EXPECT_EQ(accessCtrl->taskAssignment.header.clientExField, "clientExField1");
    EXPECT_EQ(accessCtrl->header.receiver, "sender1");
    EXPECT_EQ(accessCtrl->header.clientExField, "clientExField1");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeReceiver, TaskStop) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender2";
    workFlow.clientExField = "clientExField2";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTOP;
    accessCtrl->taskStop.header.receiver = "oldReceiver";
    accessCtrl->taskStop.header.clientExField = "oldClientExField";

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->taskStop.header.receiver, "sender2");
    EXPECT_EQ(accessCtrl->taskStop.header.clientExField, "clientExField2");
    EXPECT_EQ(accessCtrl->header.receiver, "sender2");
    EXPECT_EQ(accessCtrl->header.clientExField, "clientExField2");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeReceiver, UserInput) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender3";
    workFlow.clientExField = "clientExField3";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
    accessCtrl->userInput.header.receiver = "oldReceiver";
    accessCtrl->userInput.header.clientExField = "oldClientExField";

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->userInput.header.receiver, "sender3");
    EXPECT_EQ(accessCtrl->userInput.header.clientExField, "clientExField3");
    EXPECT_EQ(accessCtrl->header.receiver, "sender3");
    EXPECT_EQ(accessCtrl->header.clientExField, "clientExField3");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeReceiver, UserOutput) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender4";
    workFlow.clientExField = "clientExField4";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
    accessCtrl->userOutput.header.receiver = "oldReceiver";
    accessCtrl->userOutput.header.clientExField = "oldClientExField";

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->userOutput.header.receiver, "sender4");
    EXPECT_EQ(accessCtrl->userOutput.header.clientExField, "clientExField4");
    EXPECT_EQ(accessCtrl->header.receiver, "sender4");
    EXPECT_EQ(accessCtrl->header.clientExField, "clientExField4");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeReceiver, DataExchange) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender5";
    workFlow.clientExField = "clientExField5";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    accessCtrl->dataExchange.header.receiver = "oldReceiver";
    accessCtrl->dataExchange.header.clientExField = "oldClientExField";

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->dataExchange.header.receiver, "sender5");
    EXPECT_EQ(accessCtrl->dataExchange.header.clientExField, "clientExField5");
    EXPECT_EQ(accessCtrl->header.receiver, "sender5");
    EXPECT_EQ(accessCtrl->header.clientExField, "clientExField5");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeReceiver, TaskStatusManagement) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender6";
    workFlow.clientExField = "clientExField6";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    accessCtrl->taskStatusManagement.header.receiver = "oldReceiver";
    accessCtrl->taskStatusManagement.header.clientExField = "oldClientExField";

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->taskStatusManagement.header.receiver, "sender6");
    EXPECT_EQ(accessCtrl->taskStatusManagement.header.clientExField, "clientExField6");
    EXPECT_EQ(accessCtrl->header.receiver, "sender6");
    EXPECT_EQ(accessCtrl->header.clientExField, "clientExField6");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeReceiver, LoginOut) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender7";
    workFlow.clientExField = "clientExField7";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::LOGINOUT;
    accessCtrl->loginOut.header.receiver = "oldReceiver";
    accessCtrl->loginOut.header.clientExField = "oldClientExField";

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->loginOut.header.receiver, "sender7");
    EXPECT_EQ(accessCtrl->loginOut.header.clientExField, "clientExField7");
    EXPECT_EQ(accessCtrl->header.receiver, "sender7");
    EXPECT_EQ(accessCtrl->header.clientExField, "clientExField7");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestChangeReceiver, DefaultCase) {
    // Given
    WorkFlow workFlow;
    workFlow.sender = "sender8";
    workFlow.clientExField = "clientExField8";
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = static_cast<SYSAGENT::EventType>(999); // Invalid event type

    // When
    workFlow.changeReceiver(accessCtrl);

    // Then
    EXPECT_EQ(accessCtrl->header.receiver, "");
    EXPECT_EQ(accessCtrl->header.clientExField, "");

    // Clean
    GlobalMockObject::verify();
}

