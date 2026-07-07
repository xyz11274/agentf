/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetDouble: public ::testing::Test
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

TEST_F(TestGetDouble, memberExistsAndIsDouble) {
    // Given
    Json::Value node;
    node["number"] = 42.0;
    string member = "number";
    double value;

    // When
    bool result = GetDouble(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value, 42.0);

}

TEST_F(TestGetDouble, memberExistsButNotDouble) {
    // Given
    Json::Value node;
    node["number"] = "forty-two";
    string member = "number";
    double value;

    // When
    bool result = GetDouble(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestGetDouble, memberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "nonexistent";
    double value;

    // When
    bool result = GetDouble(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

