///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "queue/SystemMessageReadQueueHandler.h"
//#include "TeHelper.h"
//#include "mm_sdk.h"
//#include "data_source.h"
//#include "memory/Location.h"
//
//class TestRecoverSessionFromAccessCtrl: public ::testing::Test
//{
//protected:
//   static void SetUpTestCase()
//   {
//   }
//
//   static void TearDownTestCase()
//   {
//   }
//
//   virtual void SetUp()
//   {
//   }
//
//   virtual void TearDown()
//   {
//       GlobalMockObject::verify();
//   }
//};
//
//TEST_F(TestRecoverSessionFromAccessCtrl, recoverSessionFromAccessCtrl_NullData) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = nullptr;
//
//    // When
//    PerSession* session = SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//}
//
//TEST_F(TestRecoverSessionFromAccessCtrl, recoverSessionFromAccessCtrl_EmptyTEid) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.header.teamEnvironmentID = "";
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().with(data).will(returnValue(std::string("")));
//
//    // When
//    PerSession* session = SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRecoverSessionFromAccessCtrl, recoverSessionFromAccessCtrl_TEidNotFound) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.header.teamEnvironmentID = "nonexistent";
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().with(data).will(returnValue(std::string("nonexistent")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().with("nonexistent").will(returnValue(nullptr));
//
//    // When
//    PerSession* session = SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRecoverSessionFromAccessCtrl, recoverSessionFromAccessCtrl_SessionNotFound) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.header.teamEnvironmentID = "existent";
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().with(data).will(returnValue(std::string("existent")));
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>();
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().with("existent").will(returnValue(te));
//    MOCKER_CPP(&TeamEnvironment::getSession, PerSession* (TeamEnvironment::*)()).stubs().will(returnValue(nullptr));
//
//    // When
//    PerSession* session = SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRecoverSessionFromAccessCtrl, recoverSessionFromAccessCtrl_Success) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.header.teamEnvironmentID = "existent";
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().with(data).will(returnValue(std::string("existent")));
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>();
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().with("existent").will(returnValue(te));
//    PerSession* expectedSession = new PerSession();
//    MOCKER_CPP(&TeamEnvironment::getSession, PerSession* (TeamEnvironment::*)()).stubs().will(returnValue(expectedSession));
//
//    // When
//    PerSession* session = SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, expectedSession);
//
//    // Clean
//    GlobalMockObject::verify();
//    delete expectedSession;
//}
//
//TEST_F(TestRecoverSessionFromAccessCtrl, recoverSessionFromAccessCtrl_Exception) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.header.teamEnvironmentID = "existent";
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractTEidFromMessage)).stubs().with(data).willThrow(std::runtime_error("test exception"));
//
//    // When
//    PerSession* session = SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl(data);
//
//    // Then
//    EXPECT_EQ(session, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
