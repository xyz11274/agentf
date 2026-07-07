/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetUInt64: public ::testing::Test
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

TEST_F(TestGetUInt64, MemberExistsAndIsUInt64) {
    // Given
    Json::Value node;
    node["member"] = Json::UInt64(123456789012345ULL);  // 明确指定类型;
    string member = "member";
    unsigned long long value;

    // When
    bool result = GetUInt64(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, 123456789012345ULL);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUInt64, MemberExistsButNotUInt64) {
    // Given
    Json::Value node;
    node["member"] = "not a uint64";
    string member = "member";
    unsigned long long value;

    // When
    bool result = GetUInt64(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUInt64, MemberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "nonexistent";
    unsigned long long value;

    // When
    bool result = GetUInt64(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestGetUInt64, ExceptionDuringIsMember) {
    // Given
    Json::Value node;
    string member = "member";
    unsigned long long value;

    // Mock: `IsMember` to throw exception
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&)>(&IsMember))
    .stubs()
    .will(returnValue(false));

    // When
    bool result = GetUInt64(node, member, value);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

