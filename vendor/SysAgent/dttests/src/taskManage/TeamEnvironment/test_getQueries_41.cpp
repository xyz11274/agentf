/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <iostream>
#include "TeamEnvironment.h"
#include "taskManage/TeamEnvironment.h"
#include "mm_customedreact_llm_component.h"

class TestGetQueries: public ::testing::Test
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

TEST_F(TestGetQueries, EmptyQueries) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);

    // When
    const std::deque<std::string>& queries = teamEnvironment.getQueries();

    // Then
    EXPECT_EQ(queries.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetQueries, SingleQuery) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    teamEnvironment.addQuery("query1");

    // When
    const std::deque<std::string>& queries = teamEnvironment.getQueries();

    // Then
    EXPECT_EQ(queries.size(), 1);
    EXPECT_EQ(queries.front(), "query1");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetQueries, MultipleQueries) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    teamEnvironment.addQuery("query1");
    teamEnvironment.addQuery("query2");
    teamEnvironment.addQuery("query3");

    // When
    const std::deque<std::string>& queries = teamEnvironment.getQueries();

    // Then
    EXPECT_EQ(queries.size(), 3);
    EXPECT_EQ(queries.at(0), "query1");
    EXPECT_EQ(queries.at(1), "query2");
    EXPECT_EQ(queries.at(2), "query3");

    // Clean
    GlobalMockObject::verify();
}

