/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mokc.h"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace SYSAGENT;

class TestSetSender: public ::testing::Test
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

TEST_F(TestSetSender, setSender_TaskAssignment) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.taskAssignment.header.sender, sender);
}

TEST_F(TestSetSender, setSender_UserInput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USERINPUT;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.userInput.header.sender, sender);
}

TEST_F(TestSetSender, setSender_DataExchange) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::DATAEXCHANGE;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.dataExchange.header.sender, sender);
}

TEST_F(TestSetSender, setSender_TaskStatusManagement) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.taskStatusManagement.header.sender, sender);
}

TEST_F(TestSetSender, setSender_UserOutput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USEROUTPUT;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.userOutput.header.sender, sender);
}

TEST_F(TestSetSender, setSender_TaskStop) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTOP;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.taskStop.header.sender, sender);
}

TEST_F(TestSetSender, setSender_LoginOut) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOGINOUT;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.loginOut.header.sender, sender);
}

TEST_F(TestSetSender, setSender_Location) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOCATION;
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
    EXPECT_EQ(accessCtrl.locationInfo.header.sender, sender);
}

TEST_F(TestSetSender, setSender_Default) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = static_cast<SYSAGENT::EventType>(999); // Unknown event type
    std::string sender = "testSender";

    // When
    accessCtrl.setSender(sender);

    // Then
    EXPECT_EQ(accessCtrl.header.sender, sender);
}

