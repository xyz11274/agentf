/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_parse.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat;

class TestToJson: public ::testing::Test
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

TEST_F(TestToJson, ut_with_user_output_text) {
    // Given
    DataExchange dataExchange;
    dataExchange.userOutputText = "Hello, World!";
    Json::Value jsonObj;

    // When
    dataExchange.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "DataExchange");
    EXPECT_EQ(jsonObj["UserOutputText"].asString(), "Hello, World!");

}

TEST_F(TestToJson, ut_without_user_output_text) {
    // Given
    DataExchange dataExchange;
    Json::Value jsonObj;

    // When
    dataExchange.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "DataExchange");
    EXPECT_EQ(jsonObj["UserOutputText"].asString(), "");

}

