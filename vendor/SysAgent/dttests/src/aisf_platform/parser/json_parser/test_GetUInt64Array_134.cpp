/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetUInt64Array: public ::testing::Test
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

TEST_F(TestGetUInt64Array, memberNotFound) {
    // Given
    Json::Value node;
    string member = "uint64";
    vector<unsigned long long> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), smirror("uint64")).will(returnValue(false));

    // When
    bool ret = GetUInt64Array(node, member, value);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUInt64Array, memberFoundButNotArray) {
    // Given
    Json::Value node;
    node["uint64"] = 123;
    string member = "uint64";
    vector<unsigned long long> value;
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), smirror("uint64")).will(returnValue(true));
    MOCKER(&IsMember).stubs().will(returnValue(true));

    // When
    bool ret = GetUInt64Array(node, member, value);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUInt64Array, memberFoundAndArray) {
    // Given
    Json::Value node;
    node["uint64"] = Json::Value(Json::arrayValue);
    node["uint64"].append(123);
    node["uint64"].append(456);
    string member = "uint64";
    vector<unsigned long long> value;
    vector<unsigned long long> expected = {123, 456};
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), smirror("uint64")).will(returnValue(true));
    MOCKER(&IsMember).stubs().will(returnValue(true));

    // When
    bool ret = GetUInt64Array(node, member, value);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(value, expected);

    // Clean
    GlobalMockObject::verify();
}

