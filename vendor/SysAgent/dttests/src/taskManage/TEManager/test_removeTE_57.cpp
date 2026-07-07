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

class TestRemoveTE: public ::testing::Test
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

//TEST_F(TestRemoveTE, TeamIdExists) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>();
//    UserInfo userInfo;
//    userInfo.userName = "testUser";
//    te->setUserInfo(userInfo);
//    teManager.teamIdToTE_["testTeamId"] = te;
//    teManager.userIdToTeamId_["testUser"] = "testTeamId";
//
//    // When
//    bool result = teManager.removeTE("testTeamId");
//
//    // Then
//    EXPECT_EQ(result, true);
//    EXPECT_EQ(teManager.teamIdToTE_.count("testTeamId"), 0);
//    EXPECT_EQ(teManager.userIdToTeamId_.count("testUser"), 0);
//
//    // Clean
//    GlobalMockObject::verify();
//}

TEST_F(TestRemoveTE, TeamIdDoesNotExist) {
    // Given
    TEManager& teManager = TEManager::getInstance();

    // When
    bool result = teManager.removeTE("nonExistentTeamId");

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

