/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetUInt: public ::testing::Test
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

TEST_F(TestGetUInt, memberExistsAndIsUInt) {
    // Given
    Json::Value node;
    node["member"] = 42;
    string member = "member";
    unsigned value;

    // When
    bool ret = GetUInt(node, member, value);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(value, 42u);

}

TEST_F(TestGetUInt, memberExistsButNotUInt) {
    // Given
    Json::Value node;
    node["member"] = "not a uint";
    string member = "member";
    unsigned value;

    // When
    bool ret = GetUInt(node, member, value);

    // Then
    EXPECT_EQ(ret, false);

}

TEST_F(TestGetUInt, memberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "nonexistent";
    unsigned value;

    // When
    bool ret = GetUInt(node, member, value);

    // Then
    EXPECT_EQ(ret, false);

}

