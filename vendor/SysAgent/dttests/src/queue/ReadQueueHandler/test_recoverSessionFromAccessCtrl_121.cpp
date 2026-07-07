/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/ReadQueueHandler.h"
#include <TeamEnvironment.h>
#include "TEManager.h"
#include "TeHelper.h"
#include "message/ServerEntity.h"
#include <cstring>
#include "queue/QueueManager.h"

class TestRecoverSessionFromAccessCtrl: public ::testing::Test
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

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_NullPointer) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = nullptr;

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_TaskAssignment) {
    // Given

    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    access->taskAssignment.header.teamEnvironmentID = "TE123";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "TE123");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_TaskStop) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKSTOP;
    access->taskStop.header.teamEnvironmentID = "TE456";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "TE456");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_UserInput) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::USERINPUT;
    access->userInput.header.teamEnvironmentID = "TE789";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "TE789");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_UserOutput) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::USEROUTPUT;
    access->userOutput.header.teamEnvironmentID = "TE101";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "TE101");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_DataExchange) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    access->dataExchange.header.teamEnvironmentID = "TE202";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "TE202");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_TaskStatusManagement) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    access->taskStatusManagement.header.teamEnvironmentID = "TE303";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "TE303");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_LoginOut) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::LOGINOUT;
    access->loginOut.header.teamEnvironmentID = "TE404";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "TE404");

}

TEST_F(TestRecoverSessionFromAccessCtrl, extractTEidFromMessage_UnhandledEventType) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = static_cast<SYSAGENT::EventType>(999);
    access->taskAssignment.header.teamEnvironmentID = "TE404";

    // When
    std::string result = TEHelper::extractTEidFromMessage(access);

    // Then
    EXPECT_EQ(result, "");

}

TEST_F(TestRecoverSessionFromAccessCtrl, getTEById_EmptyId) {
    // Given
    std::string teamEnvId = "";

    // When
    std::shared_ptr<TeamEnvironment> result = TEManager::getInstance().getTEById(teamEnvId);

    // Then
    EXPECT_EQ(result, nullptr);

}

TEST_F(TestRecoverSessionFromAccessCtrl, getTEById_IdNotFound) {
    // Given
    std::string teamEnvId = "TE505";

    // When
    std::shared_ptr<TeamEnvironment> result = TEManager::getInstance().getTEById(teamEnvId);

    // Then
    EXPECT_EQ(result, nullptr);

}

TEST_F(TestRecoverSessionFromAccessCtrl, getTEById_IdFound) {
    // Given
    std::string teamEnvId = "TE606";
    UserInfo userInfo1{};
    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>(teamEnvId, userInfo1);
    TEManager::getInstance().teamIdToTE_[teamEnvId] = te;

    // When
    std::shared_ptr<TeamEnvironment> result = TEManager::getInstance().getTEById(teamEnvId);

    // Then
    EXPECT_EQ(result, te);

    // Clean
    TEManager::getInstance().teamIdToTE_.clear();
}

