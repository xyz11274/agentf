/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetInt64: public ::testing::Test
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

TEST_F(TestGetInt64, MemberExistsAndIsInt64) {
    // Given
    Json::Value node;
    node["member"] = Json::Value::Int64(123456789012345LL);
    string member = "member";
    long long int value;

    // When
    bool result = GetInt64(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, 123456789012345LL);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetInt64, MemberExistsButNotInt64) {
    // Given
    Json::Value node;
    node["member"] = Json::Value("not an int64");
    string member = "member";
    long long int value;

    // When
    bool result = GetInt64(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetInt64, MemberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "nonexistent";
    long long int value;

    // When
    bool result = GetInt64(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

