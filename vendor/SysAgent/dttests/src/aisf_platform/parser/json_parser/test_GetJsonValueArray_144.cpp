/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestGetJsonValueArray: public ::testing::Test
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

TEST_F(TestGetJsonValueArray, NodeIsNull) {
    // Given
    Json::Value node;
    string member = "test";
    vector<const Json::Value*> value;

    // When
    bool result = GetJsonValueArray(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestGetJsonValueArray, MemberNotFound) {
    // Given
    Json::Value node;
    node["other"] = Json::Value(1);
    string member = "test";
    vector<const Json::Value*> value;

    // When
    bool result = GetJsonValueArray(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestGetJsonValueArray, MemberNotArray) {
    // Given
    Json::Value node;
    node["test"] = Json::Value(1);
    string member = "test";
    vector<const Json::Value*> value;

    // When
    bool result = GetJsonValueArray(node, member, value);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestGetJsonValueArray, MemberIsArray) {
    // Given
    Json::Value node;
    node["test"] = Json::arrayValue;
    node["test"].append(Json::Value(1));
    node["test"].append(Json::Value(2));
    string member = "test";
    vector<const Json::Value*> value;

    // When
    bool result = GetJsonValueArray(node, member, value);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(value.size(), 2);
    EXPECT_EQ(value[0]->asInt(), 1);
    EXPECT_EQ(value[1]->asInt(), 2);

}

