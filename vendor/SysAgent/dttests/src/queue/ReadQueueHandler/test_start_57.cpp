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

class TestStart: public ::testing::Test
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

//TEST_F(TestStart, DataIsNull) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = nullptr;
//
//    // When
//    PerSession* session = ReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//}
//
//TEST_F(TestStart, ExtractTEidFails) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
//    data->eventType = SYSAGENT::EventType::UNKNOWN;
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().will(returnValue(""));
//
//    // When
//    PerSession* session = ReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, GetTEByIdFails) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
//    data->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    data->taskAssignment.header.teamEnvironmentID = "te_id";
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().will(returnValue("te_id"));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::getTEById)).stubs().will(returnValue(nullptr));
//
//    // When
//    PerSession* session = ReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, GetSessionFails) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
//    data->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    data->taskAssignment.header.teamEnvironmentID = "te_id";
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>("te_id", UserInfo());
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().will(returnValue("te_id"));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::getTEById)).stubs().will(returnValue(te));
//    MOCKER_CPP(&TeamEnvironment::getSession, PerSession* (TeamEnvironment::*)()).stubs().will(returnValue(nullptr));
//
//    // When
//    PerSession* session = ReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, Success) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
//    data->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    data->taskAssignment.header.teamEnvironmentID = "te_id";
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>("te_id", UserInfo());
//    PerSession session;
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().will(returnValue("te_id"));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::getTEById)).stubs().will(returnValue(te));
//    MOCKER_CPP(&TeamEnvironment::getSession, PerSession* (TeamEnvironment::*)()).stubs().will(returnValue(&session));
//
//    // When
//    PerSession* sessionResult = ReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(sessionResult, &session);
//
//    // Clean
//    GlobalMockObject::verify();
//}

