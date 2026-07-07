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

TEST_F(TestToPrettyString, validJson) {
    // Given
    string jsonStr = "{\"name\":\"Alice\", \"age\":30}";
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&Parse)).stubs().with(outBound(jsonStr.c_str()), eq(jsonStr.size()), any()).will(returnValue(true));

    // When
    string result = ToPrettyString(jsonStr);

    // Then
    EXPECT_EQ(result, "null");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestToPrettyString, invalidJson) {
    // Given
    string jsonStr = "{\"name\":\"Alice\", \"age\":30";
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&Parse)).stubs().with(outBound(jsonStr.c_str()), eq(jsonStr.size()),any()).will(returnValue(false));

    // When
    string result = ToPrettyString(jsonStr);

    // Then
    EXPECT_EQ(result, jsonStr);

    // Clean
    GlobalMockObject::verify();
}

