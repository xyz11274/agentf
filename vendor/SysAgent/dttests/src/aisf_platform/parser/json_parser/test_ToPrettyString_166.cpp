/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestToPrettyString: public ::testing::Test
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

TEST_F(TestToPrettyString, ut_emptyObject) {
    // Given
    Json::Value root;

    // When
    string ret = ToPrettyString(root);

    // Then
    EXPECT_EQ(ret, "null");

}

TEST_F(TestToPrettyString, ut_simpleObject) {
    // Given
    Json::Value root;
    root["key"] = "value";

    // When
    string ret = ToPrettyString(root);

    // Then
    EXPECT_EQ(ret, "{\n    \"key\" : \"value\"\n}");

}

TEST_F(TestToPrettyString, ut_nestedObject) {
    // Given
    Json::Value root;
    root["key1"] = "value1";
    Json::Value nested;
    nested["key2"] = "value2";
    root["nested"] = nested;

    // When
    string ret = ToPrettyString(root);

    // Then
    EXPECT_EQ(ret, "{\n    \"key1\" : \"value1\",\n    \"nested\" : \n    {\n        \"key2\" : \"value2\"\n    }\n}");

}

TEST_F(TestToPrettyString, ut_array) {
    // Given
    Json::Value root;
    root.append("item1");
    root.append("item2");

    // When
    string ret = ToPrettyString(root);

    // Then
    EXPECT_EQ(ret, "[\n    \"item1\",\n    \"item2\"\n]");

}

TEST_F(TestToPrettyString, ut_mixedTypes) {
    // Given
    Json::Value root;
    root["string"] = "value";
    root["number"] = 123;
    root["boolean"] = true;
    root["null"] = Json::nullValue;

    // When
    string ret = ToPrettyString(root);

    // Then
    EXPECT_EQ(ret, "{\n    \"boolean\" : true,\n    \"null\" : null,\n    \"number\" : 123,\n    \"string\" : \"value\"\n}");

}

