/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <iostream>
#include "TeamEnvironment.h"
#include "taskManage/TeamEnvironment.h"
#include "mm_customedreact_llm_component.h"

class TestCreateTE: public ::testing::Test
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

TEST_F(TestCreateTE, validInput) {
    // Given
    std::string teamId = "team123";
    UserInfo userInfo = {"Alice", "device1", "category1", "uuid1", "session1", "acm1"};

    // When
    std::shared_ptr<TeamEnvironment> teamEnvironment = TeamEnvironment::createTE(teamId, userInfo);

    // Then
    EXPECT_EQ(teamEnvironment->getTeamId(), teamId);
    EXPECT_EQ(teamEnvironment->getUserInfo().userName, userInfo.userName);
    EXPECT_EQ(teamEnvironment->getUserInfo().deviceId, userInfo.deviceId);
    EXPECT_EQ(teamEnvironment->getUserInfo().deviceCategory, userInfo.deviceCategory);
    EXPECT_EQ(teamEnvironment->getUserInfo().uuid, userInfo.uuid);
    EXPECT_EQ(teamEnvironment->getUserInfo().clientSessionId, userInfo.clientSessionId);
    EXPECT_EQ(teamEnvironment->getUserInfo().acmSessionId, userInfo.acmSessionId);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestCreateTE, emptyTeamId) {
    // Given
    std::string teamId = "";
    UserInfo userInfo = {"Alice", "device1", "category1", "uuid1", "session1", "acm1"};

    // When
    std::shared_ptr<TeamEnvironment> teamEnvironment = TeamEnvironment::createTE(teamId, userInfo);

    // Then
    EXPECT_EQ(teamEnvironment->getTeamId(), teamId);
    EXPECT_EQ(teamEnvironment->getUserInfo().userName, userInfo.userName);
    EXPECT_EQ(teamEnvironment->getUserInfo().deviceId, userInfo.deviceId);
    EXPECT_EQ(teamEnvironment->getUserInfo().deviceCategory, userInfo.deviceCategory);
    EXPECT_EQ(teamEnvironment->getUserInfo().uuid, userInfo.uuid);
    EXPECT_EQ(teamEnvironment->getUserInfo().clientSessionId, userInfo.clientSessionId);
    EXPECT_EQ(teamEnvironment->getUserInfo().acmSessionId, userInfo.acmSessionId);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestCreateTE, emptyUserInfo) {
    // Given
    std::string teamId = "team123";
    UserInfo userInfo = {};

    // When
    std::shared_ptr<TeamEnvironment> teamEnvironment = TeamEnvironment::createTE(teamId, userInfo);

    // Then
    EXPECT_EQ(teamEnvironment->getTeamId(), teamId);
    EXPECT_EQ(teamEnvironment->getUserInfo().userName, "");
    EXPECT_EQ(teamEnvironment->getUserInfo().deviceId, "");
    EXPECT_EQ(teamEnvironment->getUserInfo().deviceCategory, "");
    EXPECT_EQ(teamEnvironment->getUserInfo().uuid, "");
    EXPECT_EQ(teamEnvironment->getUserInfo().clientSessionId, "");
    EXPECT_EQ(teamEnvironment->getUserInfo().acmSessionId, "");

    // Clean
    GlobalMockObject::verify();
}

