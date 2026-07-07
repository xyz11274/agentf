/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetInt64Array: public ::testing::Test
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

TEST_F(TestGetInt64Array, memberNotFound) {
    // Given
    Json::Value node;
    string member = "testMember";
    vector<long long int> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember))
        .stubs().with(outBound(node), eq(member))
        .will(returnValue(false));

    // When
    bool ret = GetInt64Array(node, member, value);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetInt64Array, memberFoundButNotArray) {
    // Given
    Json::Value node;
    node["testMember"] = 123;
    string member = "testMember";
    vector<long long int> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool ret = GetInt64Array(node, member, value);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetInt64Array, memberFoundAndArrayButInvalidType) {
    // Given
    Json::Value node;
    node["testMember"] = Json::Value(Json::arrayValue);
    node["testMember"].append("notInt64");
    string member = "testMember";
    vector<long long int> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool ret = GetInt64Array(node, member, value);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetInt64Array, memberFoundAndValidArray) {
    // Given
    Json::Value node;
    node["testMember"] = Json::Value(Json::arrayValue);
    node["testMember"].append(123);
    node["testMember"].append(456);
    string member = "testMember";
    vector<long long int> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool ret = GetInt64Array(node, member, value);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(value.size(), 2);
    EXPECT_EQ(value[0], 123);
    EXPECT_EQ(value[1], 456);

    // Clean
    GlobalMockObject::verify();
}

