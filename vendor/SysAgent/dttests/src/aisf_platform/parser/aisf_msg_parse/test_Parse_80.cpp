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

TEST_F(TestParse, ut_noMembers) {
    // Given
    Json::Value root;
    MsgPayload msgPayload;

    // When
    bool result = msgPayload.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(msgPayload.function, "");
    EXPECT_EQ(msgPayload.action, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_onlyFunction) {
    // Given
    Json::Value root;
    root["Function"] = "testFunction";
    MsgPayload msgPayload;

    // When
    bool result = msgPayload.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(msgPayload.function, "testFunction");
    EXPECT_EQ(msgPayload.action, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_onlyAction) {
    // Given
    Json::Value root;
    root["Action"] = "testAction";
    MsgPayload msgPayload;

    // When
    bool result = msgPayload.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(msgPayload.function, "");
    EXPECT_EQ(msgPayload.action, "testAction");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_bothFunctionAndAction) {
    // Given
    Json::Value root;
    root["Function"] = "testFunction";
    root["Action"] = "testAction";
    MsgPayload msgPayload;

    // When
    bool result = msgPayload.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(msgPayload.function, "testFunction");
    EXPECT_EQ(msgPayload.action, "testAction");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_mockGetStringFunctionMissing) {
    // Given
    Json::Value root;
    root["Action"] = "testAction";
    MsgPayload msgPayload;
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().with(any(), eq("Function"), any()).will(returnValue(false));
    MOCKER(&JsonParser::GetString).stubs().will(returnValue(false));

    // When
    bool result = msgPayload.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(msgPayload.function, "");
    EXPECT_EQ(msgPayload.action, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_mockGetStringActionMissing) {
    // Given
    Json::Value root;
    root["Function"] = "testFunction";
    MsgPayload msgPayload;
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().with(any(), eq("Action"), any()).will(returnValue(false));
    MOCKER(&JsonParser::GetString).stubs().will(returnValue(false));

    // When
    bool result = msgPayload.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(msgPayload.function, "");
    EXPECT_EQ(msgPayload.action, "");

    // Clean
    GlobalMockObject::verify();
}

