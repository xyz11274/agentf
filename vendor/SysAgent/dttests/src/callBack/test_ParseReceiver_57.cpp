/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "callBack/callBack.h"
#include "queue/QueueManager.h"
#include "TeHelper.h"
#include "TEManager.h"
#include "aisf_platform/common_define.h"
#include <iostream>
#include "AgentManager.h"

class TestParseReceiver: public ::testing::Test
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

TEST_F(TestParseReceiver, NullAccessPtr) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> access = nullptr;

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "Unknown");

}

TEST_F(TestParseReceiver, TaskAssignmentEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    access->taskAssignment.header.receiver = "TaskAssignReceiver";

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "TaskAssignReceiver");

}

TEST_F(TestParseReceiver, TaskStopEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKSTOP;
    access->taskStop.header.receiver = "TaskStopReceiver";

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "TaskStopReceiver");

}

TEST_F(TestParseReceiver, UserInputEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::USERINPUT;
    access->userInput.header.receiver = "UserInputReceiver";

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "UserInputReceiver");

}

TEST_F(TestParseReceiver, UserOutputEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::USEROUTPUT;
    access->userOutput.header.receiver = "UserOutputReceiver";

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "UserOutputReceiver");

}

TEST_F(TestParseReceiver, DataExchangeEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    access->dataExchange.header.receiver = "DataExchangeReceiver";

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "DataExchangeReceiver");

}

TEST_F(TestParseReceiver, TaskStatusManagementEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    access->taskStatusManagement.header.receiver = "TaskStatusManagementReceiver";

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "TaskStatusManagementReceiver");

}

TEST_F(TestParseReceiver, LoginOutEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::LOGINOUT;
    access->loginOut.header.receiver = "LoginOutReceiver";

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "LoginOutReceiver");

}

TEST_F(TestParseReceiver, UnknownEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = static_cast<SYSAGENT::EventType>(999);

    // When
    std::string result = SysCallBack::ParseReceiver(access);

    // Then
    EXPECT_EQ(result, "Unknown");

}

