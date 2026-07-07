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

class TestCreateTEFromData: public ::testing::Test
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

//TEST_F(TestCreateTEFromData, NullData) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = nullptr;
//
//    // When
//    TEManager teManager;
//    auto result = teManager.createTEFromData(data);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateTEFromData, EmptyTeamEnvIdNoExistingTE) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::UNKNOWN;
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    UserInfo userInfo = {"testUser", "device1", "category1", "uuid1", "client1", "acm1"};
//    MOCKER_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(""));
//    MOCKER_CPP(&TEManager::extractUserInfo, UserInfo (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(userInfo));
//    MOCKER_CPP(&TEManager::findTEByUserId, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(nullptr));
//    MOCKER_CPP(&TeamEnvironment::createTE, std::shared_ptr<TeamEnvironment> (*)(const std::string&, const UserInfo&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>("generatedId", userInfo)));
//    MOCKER_CPP(&TEManager::generateTeamId, std::string (TEManager::*)(const UserInfo&)).stubs().will(returnValue("generatedId"));
//
//    // When
//    TEManager teManager;
//    auto result = teManager.createTEFromData(data);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->getTeamId(), "generatedId");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateTEFromData, NonEmptyTeamEnvIdExistingTE) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.header.teamEnvironmentID = "existingId";
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    auto existingTE = std::make_shared<TeamEnvironment>("existingId", UserInfo{"testUser", "device1", "category1", "uuid1", "client1", "acm1"});
//    MOCKER_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue("existingId"));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(existingTE));
//
//    // When
//    TEManager teManager;
//    auto result = teManager.createTEFromData(data);
//
//    // Then
//    EXPECT_EQ(result, existingTE);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateTEFromData, NonEmptyTeamEnvIdNoExistingTE) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.header.teamEnvironmentID = "newId";
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    UserInfo userInfo = {"testUser", "device1", "category1", "uuid1", "client1", "acm1"};
//    MOCKER_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue("newId"));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(nullptr));
//    MOCKER_CPP(&TEManager::extractUserInfo, UserInfo (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(userInfo));
//    MOCKER_CPP(&TEManager::findTEByUserId, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(nullptr));
//    MOCKER_CPP(&TeamEnvironment::createTE, std::shared_ptr<TeamEnvironment> (*)(const std::string&, const UserInfo&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>("newId", userInfo)));
//
//    // When
//    TEManager teManager;
//    auto result = teManager.createTEFromData(data);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->getTeamId(), "newId");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateTEFromData, ExistingTEForUser) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    UserInfo userInfo = {"testUser", "device1", "category1", "uuid1", "client1", "acm1"};
//    auto existingTE = std::make_shared<TeamEnvironment>("existingId", userInfo);
//    MOCKER_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(""));
//    MOCKER_CPP(&TEManager::extractUserInfo, UserInfo (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(userInfo));
//    MOCKER_CPP(&TEManager::findTEByUserId, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(existingTE));
//
//    // When
//    TEManager teManager;
//    auto result = teManager.createTEFromData(data);
//
//    // Then
//    EXPECT_EQ(result, existingTE);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCreateTEFromData, FailedToCreateTeamEnvironment) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrl;
//    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrl);
//    UserInfo userInfo = {"testUser", "device1", "category1", "uuid1", "client1", "acm1"};
//    MOCKER_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(""));
//    MOCKER_CPP(&TEManager::extractUserInfo, UserInfo (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(userInfo));
//    MOCKER_CPP(&TEManager::findTEByUserId, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(nullptr));
//    MOCKER_CPP(&TeamEnvironment::createTE, std::shared_ptr<TeamEnvironment> (*)(const std::string&, const UserInfo&)).stubs().will(returnValue(nullptr));
//    MOCKER_CPP(&TEManager::generateTeamId, std::string (TEManager::*)(const UserInfo&)).stubs().will(returnValue("generatedId"));
//
//    // When
//    TEManager teManager;
//    auto result = teManager.createTEFromData(data);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}

