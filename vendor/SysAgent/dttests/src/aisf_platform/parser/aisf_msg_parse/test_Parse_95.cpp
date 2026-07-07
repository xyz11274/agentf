/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_parse.h"

#include "aisf_platform/utils/utils.h"
#include <mockcpp/ThrowExceptionStub.h> // 可能需要包含这个头文件
#include <stdexcept>

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

TEST_F(TestParse, ParseWithAllFieldsPresent) {
    // Given
    Json::Value root;
    root["UserInputText"] = "text";
    root["UserInputObject"] = "object";
    root["UserIntent"] = "intent";
    root["UserSpecifiedAgent"] = "agent";
    root["APPName"] = "app";
    TaskAssignment taskAssignment;

    // When
    bool result = taskAssignment.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskAssignment.userInputText, "text");
    EXPECT_EQ(taskAssignment.userInputObject, "object");
    EXPECT_EQ(taskAssignment.userIntent, "intent");
    EXPECT_EQ(taskAssignment.userSpecifiedAgent, "agent");
    EXPECT_EQ(taskAssignment.APPName, "app");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ParseWithSomeFieldsMissing) {
    // Given
    Json::Value root;
    root["UserInputText"] = "text";
    root["APPName"] = "app";
    TaskAssignment taskAssignment;

    // When
    bool result = taskAssignment.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskAssignment.userInputText, "text");
    EXPECT_EQ(taskAssignment.userInputObject, "");
    EXPECT_EQ(taskAssignment.userIntent, "");
    EXPECT_EQ(taskAssignment.userSpecifiedAgent, "");
    EXPECT_EQ(taskAssignment.APPName, "app");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ParseWithNoFieldsPresent) {
    // Given
    Json::Value root;
    TaskAssignment taskAssignment;

    // When
    bool result = taskAssignment.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskAssignment.userInputText, "");
    EXPECT_EQ(taskAssignment.userInputObject, "");
    EXPECT_EQ(taskAssignment.userIntent, "");
    EXPECT_EQ(taskAssignment.userSpecifiedAgent, "");
    EXPECT_EQ(taskAssignment.APPName, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ParseWithIsMemberThrowingException) {
    // Given
    Json::Value root;
    root["UserInputText"] = "text";
    TaskAssignment taskAssignment;

    MOCKER(static_cast<bool(*)(const Json::Value&, const std::string&)>(JsonParser::IsMember))
        .stubs()
        .will(returnValue(false));

    // When
    bool result = taskAssignment.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskAssignment.userInputText, "");
    EXPECT_EQ(taskAssignment.userInputObject, "");
    EXPECT_EQ(taskAssignment.userIntent, "");
    EXPECT_EQ(taskAssignment.userSpecifiedAgent, "");
    EXPECT_EQ(taskAssignment.APPName, "");

    // Clean
    GlobalMockObject::verify();
}