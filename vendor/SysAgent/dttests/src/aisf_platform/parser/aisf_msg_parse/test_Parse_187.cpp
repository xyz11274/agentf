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

TEST_F(TestParse, MessageTypeNotFound) {
    // Given
    Json::Value root;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(false));

    // When
    TaskStatusManagement taskStatusManagement;
    bool result = taskStatusManagement.Parse(root);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, MessageTypeFound) {
    // Given
    Json::Value root;
    root["MessageType"] = "TestMessage";
    string expectedMessageType = "";
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, int&)>(&JsonParser::GetInt)).stubs().will(returnValue(true));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));

    // When
    TaskStatusManagement taskStatusManagement;
    bool result = taskStatusManagement.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStatusManagement.messageType, expectedMessageType);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, TaskStatusCodeNotFound) {
    // Given
    Json::Value root;
    root["MessageType"] = "TestMessage";
    string expectedMessageType = "";
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, int&)>(&JsonParser::GetInt)).stubs().will(returnValue(false));

    // When
    TaskStatusManagement taskStatusManagement;
    bool result = taskStatusManagement.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStatusManagement.messageType, expectedMessageType);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, TaskStatusReportNotFound) {
    // Given
    Json::Value root;
    root["MessageType"] = "TestMessage";
    root["TaskStatusCode"] = 123;
    string expectedMessageType = "";
    int expectedTaskStatusCode = 0;
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, int&)>(&JsonParser::GetInt)).stubs().will(returnValue(true));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(false));

    // When
    TaskStatusManagement taskStatusManagement;
    bool result = taskStatusManagement.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStatusManagement.messageType, expectedMessageType);
    EXPECT_EQ(taskStatusManagement.taskStatusCode, expectedTaskStatusCode);

    // Clean
    GlobalMockObject::verify();
}

