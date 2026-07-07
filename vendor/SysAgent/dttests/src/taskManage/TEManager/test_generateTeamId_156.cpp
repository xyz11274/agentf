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

class TestGenerateTeamId: public ::testing::Test
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

//TEST_F(TestGenerateTeamId, ut_with_valid_user_info) {
//    // Given
//    UserInfo userInfo = {"Alice", "device123", "categoryA", "uuid456", "session789", "acm101"};
//    TEManager& teManager = TEManager::getInstance();
//
//    // When
//    std::string teamId = teManager.generateTeamId(userInfo);
//
//    // Then
//    EXPECT_EQ(teamId.substr(0, 3), "te_");
//    EXPECT_EQ(teamId.find(userInfo.userName), 3);
//    EXPECT_NE(teamId.find(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())), std::string::npos);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestGenerateTeamId, ut_with_empty_user_name) {
//    // Given
//    UserInfo userInfo = {"", "device123", "categoryA", "uuid456", "session789", "acm101"};
//    TEManager& teManager = TEManager::getInstance();
//
//    // When
//    std::string teamId = teManager.generateTeamId(userInfo);
//
//    // Then
//    EXPECT_EQ(teamId.substr(0, 3), "te_");
//    EXPECT_EQ(teamId.find(userInfo.userName), 3);
//    EXPECT_NE(teamId.find(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())), std::string::npos);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
