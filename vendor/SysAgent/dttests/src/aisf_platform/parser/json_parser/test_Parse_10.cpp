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

TEST_F(TestParse, nullData) {
    // Given
    Json::Value root;

    // When
    bool result = Parse(nullptr, root);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestParse, emptyData) {
    // Given
    const char* data = "";
    Json::Value root;

    // When
    bool result = Parse(data, root);

    // Then
    EXPECT_EQ(result, false);

}

TEST_F(TestParse, validJson) {
    // Given
    const char* data = "{\"key\":\"value\"}";
    Json::Value root;

    // When
    bool result = Parse(data, root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(root["key"].asString(), "value");

}

TEST_F(TestParse, invalidJson) {
    // Given
    const char* data = "{\"key\":\"value\"";
    Json::Value root;

    // When
    bool result = Parse(data, root);

    // Then
    EXPECT_EQ(result, false);

}

