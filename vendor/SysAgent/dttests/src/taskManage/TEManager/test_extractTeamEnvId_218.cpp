/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <algorithm>
#include "WorkFlow.h" // workflow
#include "taskManage/AgentManager.h"
#include "taskManage/TEManager.h"
#include "iostream"
#include <iostream>

class TestExtractTeamEnvId: public ::testing::Test
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

TEST_F(TestExtractTeamEnvId, NullData) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data;

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, TaskAssignment) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    data->taskAssignment.header.teamEnvironmentID = "TEAM123";

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "TEAM123");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, UserInput) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::USERINPUT;
    data->userInput.header.teamEnvironmentID = "TEAM456";

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "TEAM456");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, DataExchange) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    data->dataExchange.header.teamEnvironmentID = "TEAM789";

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "TEAM789");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, TaskStatusManagement) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    data->taskStatusManagement.header.teamEnvironmentID = "TEAM101";

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "TEAM101");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, UserOutput) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::USEROUTPUT;
    data->userOutput.header.teamEnvironmentID = "TEAM202";

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "TEAM202");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, LoginOut) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::LOGINOUT;
    data->loginOut.header.teamEnvironmentID = "TEAM303";

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "TEAM303");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, TaskStop) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKSTOP;
    data->taskStop.header.teamEnvironmentID = "TEAM404";

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "TEAM404");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractTeamEnvId, UnknownEventType) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = static_cast<SYSAGENT::EventType>(999);

    // When
    std::string result = TEManager::getInstance().extractTeamEnvId(data);

    // Then
    EXPECT_EQ(result, "");

    // Clean
    GlobalMockObject::verify();
}

