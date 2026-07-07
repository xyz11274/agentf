/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mokc.h"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace SYSAGENT;

class TestSetReceiver: public ::testing::Test
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

TEST_F(TestSetReceiver, setReceiver_TaskAssignment) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.taskAssignment.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_UserInput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USERINPUT;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.userInput.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_DataExchange) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::DATAEXCHANGE;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.dataExchange.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_TaskStatusManagement) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.taskStatusManagement.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_UserOutput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USEROUTPUT;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.userOutput.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_TaskStop) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTOP;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.taskStop.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_LoginOut) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOGINOUT;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.loginOut.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_Location) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOCATION;

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
    EXPECT_EQ(accessCtrl.locationInfo.header.receiver, receiver);
}

TEST_F(TestSetReceiver, setReceiver_Default) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = static_cast<SYSAGENT::EventType>(999); // Assuming 999 is not a valid event type

    // When
    std::string receiver = "testReceiver";
    accessCtrl.setReceiver(receiver);

    // Then
    EXPECT_EQ(accessCtrl.header.receiver, receiver);
}

