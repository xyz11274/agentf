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

TEST_F(TestToJson, ut_messageTypeTokenRefresh) {
    // Given
    LoginOut loginOut;
    loginOut.messageType = "TokenRefresh";
    Json::Value jsonObj;

    // When
    loginOut.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "TokenRefresh");

}

TEST_F(TestToJson, ut_messageTypeLoginOut) {
    // Given
    LoginOut loginOut;
    loginOut.messageType = "LoginOut";
    Json::Value jsonObj;

    // When
    loginOut.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "LoginOut");

}

