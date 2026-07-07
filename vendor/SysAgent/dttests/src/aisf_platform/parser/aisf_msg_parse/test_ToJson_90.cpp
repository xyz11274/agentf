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

TEST_F(TestToJson, ut_with_empty_payload) {
    // Given
    Json::Value expectedJsonObj;
    MsgPayload msgPayload;

    // When
    Json::Value jsonObj;
    msgPayload.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj, expectedJsonObj);

}

TEST_F(TestToJson, ut_with_non_empty_payload) {
    // Given
    Json::Value expectedJsonObj;
    expectedJsonObj["key"] = "value";
    MsgPayload msgPayload;
    msgPayload.originalPayload = expectedJsonObj;

    // When
    Json::Value jsonObj;
    msgPayload.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj, expectedJsonObj);

}

