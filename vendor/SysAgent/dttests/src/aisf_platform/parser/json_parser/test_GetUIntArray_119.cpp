/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetUIntArray: public ::testing::Test
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

TEST_F(TestGetUIntArray, MemberExistsAndIsArray) {
    // Given
    Json::Value node;
    node["member"] = Json::Value(Json::arrayValue);
    node["member"].append(1u);
    node["member"].append(2u);
    std::vector<unsigned> value;
    std::vector<unsigned> expectedValue = {1u, 2u};

    // When
    bool result = GetUIntArray(node, "member", value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, expectedValue);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUIntArray, MemberDoesNotExist) {
    // Given
    Json::Value node;
    std::vector<unsigned> value;

    // When
    bool result = GetUIntArray(node, "nonexistent", value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUIntArray, MemberExistsButNotArray) {
    // Given
    Json::Value node;
    node["member"] = 42;
    std::vector<unsigned> value;

    // When
    bool result = GetUIntArray(node, "member", value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUIntArray, MemberExistsAndIsArrayWithNonUInt) {
    // Given
    Json::Value node;
    node["member"] = Json::Value(Json::arrayValue);
    node["member"].append(1u);
    node["member"].append("not_uint");
    std::vector<unsigned> value;

    // When
    bool result = GetUIntArray(node, "member", value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

