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

TEST_F(TestAddQuery, AddQueryBelowMaxQueries) {
    // Given
    Rag rag;
    std::string newQuery = "query1";

    // When
    rag.add_query(newQuery);

    // Then
    EXPECT_EQ(rag.query.size(), 1);
    EXPECT_EQ(rag.query.back(), newQuery);
}

TEST_F(TestAddQuery, AddQueryAtMaxQueries) {
    // Given
    Rag rag;
    for (size_t i = 0; i < 10; ++i) {
        rag.add_query("query" + std::to_string(i));
    }
    std::string newQuery = "queryNew";

    // When
    rag.add_query(newQuery);

    // Then
    EXPECT_EQ(rag.query.size(), 10);
    EXPECT_EQ(rag.query.front(), "query1");
    EXPECT_EQ(rag.query.back(), newQuery);
}

TEST_F(TestAddQuery, AddQueryAboveMaxQueries) {
    // Given
    Rag rag;
    for (size_t i = 0; i < 10 + 1; ++i) {
        rag.add_query("query" + std::to_string(i));
    }

}