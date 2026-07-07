/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetNode: public ::testing::Test
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

TEST_F(TestGetNode, NodeIsNull) {
    // Given
    Json::Value node(Json::nullValue);
    string member = "test";

    // When
    const Json::Value* result = GetNode(node, member);

    // Then
    EXPECT_EQ(result, nullptr);

}

TEST_F(TestGetNode, MemberDoesNotExist) {
    // Given
    Json::Value node;
    node["key"] = "value";
    string member = "test";

    // When
    const Json::Value* result = GetNode(node, member);

    // Then
    EXPECT_EQ(result, nullptr);

}

TEST_F(TestGetNode, MemberExists) {
    // Given
    Json::Value node;
    node["test"] = "value";
    string member = "test";

    // When
    const Json::Value* result = GetNode(node, member);

    // Then
    EXPECT_EQ(*result, "value");

}

