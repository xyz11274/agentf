/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestToRawString: public ::testing::Test
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

TEST_F(TestToRawString, ut_emptyObject) {
    // Given
    Json::Value root;

    // When
    string ret = ToRawString(root);

    // Then
    EXPECT_EQ(ret, "null");

}

TEST_F(TestToRawString, ut_simpleObject) {
    // Given
    Json::Value root;
    root["key"] = "value";

    // When
    string ret = ToRawString(root);

    // Then
    EXPECT_EQ(ret, "{\"key\":\"value\"}");

}

TEST_F(TestToRawString, ut_nestedObject) {
    // Given
    Json::Value root;
    root["key1"] = "value1";
    Json::Value nested;
    nested["key2"] = "value2";
    root["nested"] = nested;

    // When
    string ret = ToRawString(root);

    // Then
    EXPECT_EQ(ret, "{\"key1\":\"value1\",\"nested\":{\"key2\":\"value2\"}}");

}

TEST_F(TestToRawString, ut_array) {
    // Given
    Json::Value root;
    root.append("item1");
    root.append("item2");

    // When
    string ret = ToRawString(root);

    // Then
    EXPECT_EQ(ret, "[\"item1\",\"item2\"]");

}

TEST_F(TestToRawString, ut_mixedTypes) {
    // Given
    Json::Value root;
    root["string"] = "value";
    root["number"] = 123;
    root["boolean"] = true;
    root["null"] = Json::nullValue;

    // When
    string ret = ToRawString(root);

    // Then
    EXPECT_EQ(ret, "{\"boolean\":true,\"null\":null,\"number\":123,\"string\":\"value\"}");

}

