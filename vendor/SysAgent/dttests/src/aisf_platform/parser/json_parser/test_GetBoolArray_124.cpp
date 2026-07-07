/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetBoolArray: public ::testing::Test
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

TEST_F(TestGetBoolArray, memberNotFound) {
    // Given
    Json::Value node;
    string member = "testMember";
    vector<bool> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(false));

    // When
    bool result = GetBoolArray(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetBoolArray, memberFoundAndValid) {
    // Given
    Json::Value node;
    node["testMember"] = Json::Value(Json::arrayValue);
    node["testMember"].append(true);
    node["testMember"].append(false);
    string member = "testMember";
    vector<bool> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool result = GetBoolArray(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value.size(), 2);
    EXPECT_EQ(value[0], true);
    EXPECT_EQ(value[1], false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetBoolArray, memberFoundButNotArray) {
    // Given
    Json::Value node;
    node["testMember"] = 123;
    string member = "testMember";
    vector<bool> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool result = GetBoolArray(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

