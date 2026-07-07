/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_parse.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat;

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

TEST_F(TestParse, MessageTypeExists) {
    // Given
    Json::Value root;
    root["MessageType"] = "LoginOut";
    string expectedMessageType = "LoginOut";

    // When
    LoginOut loginOut;
    bool result = loginOut.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(loginOut.messageType, expectedMessageType);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, MessageTypeDoesNotExist) {
    // Given
    Json::Value root;

    // Mock: `JsonParser::GetString`
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString))
        .stubs()
        .will(returnValue(false));

    // When
    LoginOut loginOut;
    bool result = loginOut.Parse(root);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, MessageTypeGetError1) {
    // Given
    Json::Value root;
    root["MessageType"] = Json::Value::null;

    // Mock: `JsonParser::GetString`
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString))
        .stubs()
        .will(returnValue(false));

    // When
    LoginOut loginOut;
    bool result = loginOut.Parse(root);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

