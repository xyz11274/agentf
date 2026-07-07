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

TEST_F(TestParse, emptyString) {
    // Given
    string data = "";
    Json::Value root;

    // When
    bool ret = Parse(data, root);

    // Then
    EXPECT_EQ(ret, false);

}

TEST_F(TestParse, nonEmptyString) {
    // Given
    string data = "{\"key\":\"value\"}";
    Json::Value root;

    // When
    bool ret = Parse(data, root);

    // Then
    EXPECT_EQ(ret, true);

}

TEST_F(TestParse, invalidJson1) {
    // Given
    string data = "{\"key\":\"value\"";
    Json::Value root;

    // When
    bool ret = Parse(data, root);

    // Then
    EXPECT_EQ(ret, false);

}

