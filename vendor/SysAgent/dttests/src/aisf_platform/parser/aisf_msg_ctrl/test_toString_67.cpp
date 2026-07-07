/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mokc.h"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace SYSAGENT;

class TestToString: public ::testing::Test
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

TEST_F(TestToString, toString_TaskAssignment) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::TASKASSIGNMENT;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.taskAssignment.ToJson());
}

TEST_F(TestToString, toString_TaskStop) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::TASKSTOP;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.taskStop.ToJson());
}

TEST_F(TestToString, toString_UserInput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::USERINPUT;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.userInput.ToJson());
}

TEST_F(TestToString, toString_UserOutput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::USEROUTPUT;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.userOutput.ToJson());
}

TEST_F(TestToString, toString_DataExchange) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::DATAEXCHANGE;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.dataExchange.ToJson());
}

TEST_F(TestToString, toString_TaskStatusManagement) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::TASKSTATUSMANAGEMENT;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.taskStatusManagement.ToJson());
}

TEST_F(TestToString, toString_LoginOut) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::LOGINOUT;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.loginOut.ToJson());
}

TEST_F(TestToString, toString_Location) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = EventType::LOCATION;
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, accessCtrl.locationInfo.ToJson());
}

TEST_F(TestToString, toString_Default) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = static_cast<EventType>(-1); // Invalid event type
    // When
    string result = accessCtrl.toString();
    // Then
    EXPECT_EQ(result, "");
}

