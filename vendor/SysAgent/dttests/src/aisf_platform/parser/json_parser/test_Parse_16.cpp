/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestParse: public ::testing::Test
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

TEST_F(TestParse, NullData) {
    // Given
    Json::Value root;

    // When
    bool result = Parse(nullptr, 10, root);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestParse, ZeroSize) {
    // Given
    Json::Value root;
    const char* data = "{}";

    // When
    bool result = Parse(data, 0, root);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestParse, InvalidJson) {
    // Given
    Json::Value root;
    const char* data = "{invalid json}";

    // When
    bool result = Parse(data, strlen(data), root);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestParse, ValidJson) {
    // Given
    Json::Value root;
    const char* data = "{\"key\":\"value\"}";

    // When
    bool result = Parse(data, strlen(data), root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(root["key"].asString(), "value");

}

