/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mokc.h"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace SYSAGENT;

class TestSetTaskID: public ::testing::Test
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

TEST_F(TestSetTaskID, setTaskID_TaskAssignment) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.taskAssignment.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_UserInput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USERINPUT;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.userInput.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_DataExchange) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::DATAEXCHANGE;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.dataExchange.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_TaskStatusManagement) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.taskStatusManagement.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_UserOutput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USEROUTPUT;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.userOutput.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_TaskStop) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTOP;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.taskStop.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_LoginOut) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOGINOUT;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.loginOut.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_Location) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOCATION;
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
    EXPECT_EQ(accessCtrl.locationInfo.header.taskID, taskID);
}

TEST_F(TestSetTaskID, setTaskID_Default) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = static_cast<SYSAGENT::EventType>(999); // Assuming 999 is not a valid EventType
    std::string taskID = "task123";

    // When
    accessCtrl.setTaskID(taskID);

    // Then
    EXPECT_EQ(accessCtrl.header.taskID, taskID);
}

