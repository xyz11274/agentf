/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetInt: public ::testing::Test
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

TEST_F(TestGetInt, MemberExistsAndIsInt) {
    // Given
    Json::Value node;
    node["key"] = 42;
    string member = "key";
    int value;

    // When
    bool result = GetInt(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, 42);

}

TEST_F(TestGetInt, MemberExistsButNotInt) {
    // Given
    Json::Value node;
    node["key"] = "not an int";
    string member = "key";
    int value;

    // When
    bool result = GetInt(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestGetInt, MemberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "nonexistent";
    int value;

    // When
    bool result = GetInt(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

