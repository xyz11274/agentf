/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <iostream>
#include "TeamEnvironment.h"
#include "taskManage/TeamEnvironment.h"
#include "mm_customedreact_llm_component.h"

class TestAddQuery: public ::testing::Test
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

TEST_F(TestAddQuery, addQueryBelowMaxQueries) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    std::string query1 = "query1";
    std::string query2 = "query2";

    // When
    teamEnvironment.addQuery(query1);
    teamEnvironment.addQuery(query2);

    // Then
    const std::deque<std::string>& queries = teamEnvironment.getQueries();
    EXPECT_EQ(queries.size(), 2);
    EXPECT_EQ(queries[0], query1);
    EXPECT_EQ(queries[1], query2);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddQuery, addQueryAtMaxQueries) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    for (int i = 0; i < 10; ++i) {
        teamEnvironment.addQuery("query" + std::to_string(i));
    }
    std::string newQuery = "newQuery";

    // When
    teamEnvironment.addQuery(newQuery);

    // Then
    const std::deque<std::string>& queries = teamEnvironment.getQueries();
    EXPECT_EQ(queries.size(), 10);
    EXPECT_EQ(queries[10 - 1], newQuery);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddQuery, addQueryAboveMaxQueries) {
    // Given
    UserInfo userInfo;
    TeamEnvironment teamEnvironment("team1", userInfo);
    for (int i = 0; i < 10 + 1; ++i) {
        teamEnvironment.addQuery("query" + std::to_string(i));
    }

}