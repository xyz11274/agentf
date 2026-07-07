/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <iostream>
#include "TeamEnvironment.h"
#include "taskManage/TeamEnvironment.h"
#include "mm_customedreact_llm_component.h"

class TestGetExtraInfo: public ::testing::Test
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

TEST_F(TestGetExtraInfo, KeyExists) {
    // Given
    TeamEnvironment teamEnvironment("team1", UserInfo());
    teamEnvironment.setExtraInfo("key1", "value1");

    // When
    std::string result = teamEnvironment.getExtraInfo("key1");

    // Then
    EXPECT_EQ(result, "value1");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetExtraInfo, KeyDoesNotExist) {
    // Given
    TeamEnvironment teamEnvironment("team1", UserInfo());

    // When
    std::string result = teamEnvironment.getExtraInfo("key1");

    // Then
    EXPECT_EQ(result, "");

    // Clean
    GlobalMockObject::verify();
}

