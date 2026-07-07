/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <iostream>
#include "TeamEnvironment.h"
#include "taskManage/TeamEnvironment.h"
#include "mm_customedreact_llm_component.h"

class TestUpdateUserSession: public ::testing::Test
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

TEST_F(TestUpdateUserSession, updateUserSession) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team123", userInfo);
    std::string clientSessionId = "client456";
    std::string acmSessionId = "acm789";

    // When
    teamEnvironment.updateUserSession(clientSessionId, acmSessionId);

    // Then
    EXPECT_EQ(teamEnvironment.getUserInfo().clientSessionId, clientSessionId);
    EXPECT_EQ(teamEnvironment.getUserInfo().acmSessionId, acmSessionId);

    // Clean
    GlobalMockObject::verify();
}

