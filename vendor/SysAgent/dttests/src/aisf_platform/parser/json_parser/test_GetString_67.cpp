/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetString: public ::testing::Test
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

TEST_F(TestGetString, memberExistsAndIsString) {
    // Given
    Json::Value node;
    node["key"] = "value";
    string member = "key";
    string value;
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), smirror("key")).will(returnValue(true));
    MOCKER(&IsMember).stubs().will(returnValue(true));

    // When
    bool result = GetString(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, "value");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetString, memberExistsButNotString) {
    // Given
    Json::Value node;
    node["key"] = 123;
    string member = "key";
    string value;
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), smirror("key")).will(returnValue(true));
    MOCKER(&IsMember).stubs().will(returnValue(true));

    // When
    bool result = GetString(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetString, memberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "key";
    string value;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember)).stubs().with(outBound(node), smirror("key")).will(returnValue(false));

    // When
    bool result = GetString(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

