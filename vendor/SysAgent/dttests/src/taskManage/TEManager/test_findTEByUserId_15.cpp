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

class TestFindTEByUserId: public ::testing::Test
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

TEST_F(TestFindTEByUserId, UserIdNotFound) {
    // Given
    TEManager& teManager = TEManager::getInstance();

    // When
    std::shared_ptr<TeamEnvironment> result = teManager.findTEByUserId("nonexistentUser");

    // Then
    EXPECT_EQ(result, nullptr);

    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestFindTEByUserId, TeamIdNotFound) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    teManager.userIdToTeamId_["existingUser"] = "nonexistentTeam";
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = teManager.findTEByUserId("existingUser");
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestFindTEByUserId, TeamEnvironmentFound) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    teManager.userIdToTeamId_["existingUser"] = "existingTeam";
//    auto teamEnvironment = std::make_shared<TeamEnvironment>();
//    teManager.teamIdToTE_["existingTeam"] = teamEnvironment;
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = teManager.findTEByUserId("existingUser");
//
//    // Then
//    EXPECT_EQ(result, teamEnvironment);
//
//    // Clean
//    GlobalMockObject::verify();
//}

