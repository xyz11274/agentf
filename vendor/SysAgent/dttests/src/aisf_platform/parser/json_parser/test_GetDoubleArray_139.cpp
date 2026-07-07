/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetDoubleArray: public ::testing::Test
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

TEST_F(TestGetDoubleArray, memberNotFound) {
    // Given
    Json::Value node;
    string member = "testMember";
    vector<double> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(false));

    // When
    bool result = GetDoubleArray(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetDoubleArray, memberFoundButNotArray) {
    // Given
    Json::Value node;
    node["testMember"] = 123;
    string member = "testMember";
    vector<double> value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool result = GetDoubleArray(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetDoubleArray, memberFoundAndValidArray) {
    // Given
    Json::Value node;
    node["testMember"] = Json::Value(Json::arrayValue);
    node["testMember"].append(1.1);
    node["testMember"].append(2.2);
    string member = "testMember";
    vector<double> value;
    vector<double> expectedValue = {1.1, 2.2};
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), eq(member)).will(returnValue(true));

    // When
    bool result = GetDoubleArray(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, expectedValue);

    // Clean
    GlobalMockObject::verify();
}

