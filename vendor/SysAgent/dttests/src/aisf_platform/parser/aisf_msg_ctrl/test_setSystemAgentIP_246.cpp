/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"

#include "mockcpp/mokc.h"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace SYSAGENT;

class TestSetSystemAgentIP: public ::testing::Test
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

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_TaskAssignment) {
    // Given
    std::string ip = "";
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    EXPECT_EQ(accessCtrl.taskAssignment.header.sysAgentEntity, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_UserInput) {
    // Given
    std::string ip = "192.168.1.2";
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USERINPUT;

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    EXPECT_EQ(accessCtrl.userInput.header.sysAgentEntity, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_DataExchange) {
    // Given
    std::string ip = "192.168.1.3";
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::DATAEXCHANGE;

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    EXPECT_EQ(accessCtrl.dataExchange.header.sysAgentEntity, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_TaskStatusManagement) {
    // Given
    std::string ip = "192.168.1.4";
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    EXPECT_EQ(accessCtrl.taskStatusManagement.header.sysAgentEntity, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_UserOutput) {
    // Given
    // MOCKER(static_cast<bool(*)(const string&, string&)>(&Utils::GetEnvVariable)).stubs().will(returnValue(true));
    std::string ip = "192.168.1.5";
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USEROUTPUT;

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    EXPECT_EQ(accessCtrl.userOutput.header.sysAgentEntity, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_TaskStop) {
    // Given
    // MOCKER(static_cast<bool(*)(const string&, string&)>(&Utils::GetEnvVariable)).stubs().will(returnValue(true));
    std::string ip = "192.168.1.6";
//    MOCKER_CPP((&Utils::GetEnvVariable), bool(const string&, string&)).stubs()
//          .with(eq("SYSAGENTENTITY_IP"), eq(ip)).will(returnValue(true));
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTOP;

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    EXPECT_EQ(accessCtrl.taskStop.header.sysAgentEntity, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_UnknownEventType) {
    // Given
    // MOCKER(static_cast<bool(*)(const string&, string&)>(&Utils::GetEnvVariable)).stubs().will(returnValue(true));
    std::string ip = "192.168.1.7";
    // MOCKER_CPP((&Utils::GetEnvVariable), bool(const string&, string&)).stubs()
        //  .with(eq("SYSAGENTENTITY_IP"), eq(ip)).will(returnValue(true));
    AccessCtrl accessCtrl;
    accessCtrl.eventType = static_cast<SYSAGENT::EventType>(999);

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    // No specific expectation for default case, just ensure no crash


    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetSystemAgentIP, setSystemAgentIP_GetEnvVariableFail) {
    // Given
    // MOCKER(static_cast<bool(*)(const string&, string&)>(&Utils::GetEnvVariable)).stubs().will(returnValue(true));
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;

    // When
    accessCtrl.setSystemAgentIP();

    // Then
    // No specific expectation for default case, just ensure no crash

    // Clean
    GlobalMockObject::verify();
}

