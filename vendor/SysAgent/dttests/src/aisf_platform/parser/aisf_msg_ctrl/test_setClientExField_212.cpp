/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mokc.h"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace SYSAGENT;

class TestSetClientExField: public ::testing::Test
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

TEST_F(TestSetClientExField, setClientExField_TaskAssignment) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "testField";
    // EventType eventType = SYSAGENT::EventType::TASKASSIGNMENT;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.taskAssignment.header.clientExField, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_UserInput) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "";
    // EventType eventType = SYSAGENT::EventType::USERINPUT;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.userInput.header.clientExField, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_DataExchange) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "";
    // EventType eventType = SYSAGENT::EventType::DATAEXCHANGE;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.dataExchange.header.clientExField, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_TaskStatusManagement) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "";
    // EventType eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.taskStatusManagement.header.clientExField, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_UserOutput) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "";
    // EventType eventType = SYSAGENT::EventType::USEROUTPUT;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.userOutput.header.clientExField, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_TaskStop) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "";
    // EventType eventType = SYSAGENT::EventType::TASKSTOP;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.taskStop.header.clientExField, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_LoginOut) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "";
    // EventType eventType = SYSAGENT::EventType::LOGINOUT;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.loginOut.header.clientExField, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_Location) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "";
    // EventType eventType = SYSAGENT::EventType::LOCATION;

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
    EXPECT_EQ(accessCtrl.locationInfo.header.sender, clientExField);
}

TEST_F(TestSetClientExField, setClientExField_Default) {
    // Given
    AccessCtrl accessCtrl;
    std::string clientExField = "testField";
    // EventType eventType = static_cast<SYSAGENT::EventType>(999); // Assuming 999 is not a valid EventType

    // When
    accessCtrl.setClientExField(clientExField);

    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, clientExField);
}

