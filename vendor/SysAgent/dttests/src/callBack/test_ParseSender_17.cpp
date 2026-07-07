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

class TestParseSender: public ::testing::Test
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

TEST_F(TestParseSender, NullAccessCtrl) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> access = nullptr;

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "Unknown");

}

TEST_F(TestParseSender, TaskAssignment) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    access->taskAssignment.header.sender = "TaskAssigner";

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "TaskAssigner");

}

TEST_F(TestParseSender, TaskStop) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKSTOP;
    access->taskStop.header.sender = "TaskStopper";

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "TaskStopper");

}

TEST_F(TestParseSender, UserInput) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::USERINPUT;
    access->userInput.header.sender = "UserInputter";

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "UserInputter");

}

TEST_F(TestParseSender, UserOutput) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::USEROUTPUT;
    access->userOutput.header.sender = "UserOutputter";

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "UserOutputter");

}

TEST_F(TestParseSender, DataExchange) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    access->dataExchange.header.sender = "DataExchanger";

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "DataExchanger");

}

TEST_F(TestParseSender, TaskStatusManagement) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    access->taskStatusManagement.header.sender = "TaskStatusManager";

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "TaskStatusManager");

}

TEST_F(TestParseSender, LoginOut) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::LOGINOUT;
    access->loginOut.header.sender = "LoginOutter";

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "LoginOutter");

}

TEST_F(TestParseSender, UnknownEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = static_cast<SYSAGENT::EventType>(999); // Assuming 999 is not a valid event type

    // When
    std::string result = SysCallBack::ParseSender(access);

    // Then
    EXPECT_EQ(result, "Unknown");

}

