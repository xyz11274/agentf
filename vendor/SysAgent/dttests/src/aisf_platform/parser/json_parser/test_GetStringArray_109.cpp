/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetStringArray: public ::testing::Test
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

TEST_F(TestGetStringArray, MemberExistsAndIsArray) {
    // Given
    Json::Value node;
    node["member"] = Json::Value(Json::arrayValue);
    node["member"].append("value1");
    node["member"].append("value2");
    std::vector<std::string> value;

    // When
    bool result = GetStringArray(node, "member", value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value.size(), 2);
    EXPECT_EQ(value[0], "value1");
    EXPECT_EQ(value[1], "value2");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetStringArray, MemberDoesNotExist) {
    // Given
    Json::Value node;
    std::vector<std::string> value;

    // When
    bool result = GetStringArray(node, "nonexistent", value);

    // Then
    EXPECT_EQ(result, false);
    EXPECT_EQ(value.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetStringArray, MemberExistsButNotArray) {
    // Given
    Json::Value node;
    node["member"] = "not an array";
    std::vector<std::string> value;

    // When
    bool result = GetStringArray(node, "member", value);

    // Then
    EXPECT_EQ(result, false);
    EXPECT_EQ(value.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

