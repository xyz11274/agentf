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

class TestAddTE: public ::testing::Test
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

//TEST_F(TestAddTE, ut_addTE_with_valid_team_environment) {
//    // Given
//    MOCKER_CPP(&TEManager::extractUserInfo, UserInfo (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(UserInfo{"Alice", "alice@example.com"}));
//    MOCKER_CPP(&TEManager::generateTeamId, std::string (TEManager::*)(const UserInfo&)).stubs().will(returnValue("team123"));
//
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>(accessCtrl);
//    te->setTeamId("team123");
//    te->setUserInfo(UserInfo{"Alice", "alice@example.com"});
//
//    TEManager& teManager = TEManager::getInstance();
//
//    // When
//    teManager.addTE(te);
//
//    // Then
//    EXPECT_EQ(teManager.teamIdToTE_["team123"], te);
//    EXPECT_EQ(teManager.userIdToTeamId_["Alice"], "team123");
//
//    // Clean
//    GlobalMockObject::verify();
//}

