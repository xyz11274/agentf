/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetBool: public ::testing::Test
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

TEST_F(TestGetBool, memberExistsAndIsBool) {
    // Given
    Json::Value node;
    node["member"] = true;
    string member = "member";
    bool value;

    // When
    bool result = GetBool(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, true);

}

TEST_F(TestGetBool, memberExistsButNotBool) {
    // Given
    Json::Value node;
    node["member"] = "not a bool";
    string member = "member";
    bool value;

    // When
    bool result = GetBool(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestGetBool, memberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "nonexistent";
    bool value;

    // When
    bool result = GetBool(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

