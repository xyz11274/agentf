/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <iostream>
#include "TeamEnvironment.h"
#include "taskManage/TeamEnvironment.h"
#include "mm_customedreact_llm_component.h"

class TestAddWorkFlow: public ::testing::Test
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

TEST_F(TestAddWorkFlow, AddNullWorkFlow) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    std::shared_ptr<WorkFlow> nullWorkFlow = nullptr;

    // When
    teamEnvironment.addWorkFlow(nullWorkFlow);

    // Then
    EXPECT_EQ(teamEnvironment.getWorkFlow(), nullptr);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddWorkFlow, ReplaceExistingWorkFlow) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    auto initialWorkFlow = std::make_shared<WorkFlow>();
    teamEnvironment.addWorkFlow(initialWorkFlow);
    auto newWorkFlow = std::make_shared<WorkFlow>();

    // When
    teamEnvironment.addWorkFlow(newWorkFlow);

    // Then
    EXPECT_EQ(teamEnvironment.getWorkFlow(), newWorkFlow);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddWorkFlow, AddNewWorkFlow) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    auto workFlow = std::make_shared<WorkFlow>();

    // When
    teamEnvironment.addWorkFlow(workFlow);

    // Then
    EXPECT_EQ(teamEnvironment.getWorkFlow(), workFlow);

    // Clean
    GlobalMockObject::verify();
}

