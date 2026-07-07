/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetIntArray: public ::testing::Test
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

TEST_F(TestGetIntArray, memberExistsAndIsArray) {
    // Given
    Json::Value node;
    node["member"] = Json::Value(Json::arrayValue);
    node["member"].append(1);
    node["member"].append(2);
    string member = "member";
    vector<int> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool ret = GetIntArray(node, member, value);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(value.size(), 2);
    EXPECT_EQ(value[0], 1);
    EXPECT_EQ(value[1], 2);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetIntArray, memberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "member";
    vector<int> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(false));

    // When
    bool ret = GetIntArray(node, member, value);

    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(value.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetIntArray, memberExistsButNotArray) {
    // Given
    Json::Value node;
    node["member"] = Json::Value(1);
    string member = "member";
    vector<int> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool ret = GetIntArray(node, member, value);

    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(value.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

