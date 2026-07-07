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

TEST_F(TestParse, AllFieldsPresent1) {
    // Given
    Json::Value root;
    root["UserInputText"] = "text";
    root["UserInputObject"] = "object";
    root["UserIntent"] = "intent";
    root["UserSpecifiedAgent"] = "agent";
    TaskStop taskStop;

    // When
    bool result = taskStop.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStop.userInputText, "text");
    EXPECT_EQ(taskStop.userInputObject, "object");
    EXPECT_EQ(taskStop.userIntent, "intent");
    EXPECT_EQ(taskStop.userSpecifiedAgent, "agent");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, SomeFieldsMissing1) {
    // Given
    Json::Value root;
    root["UserInputText"] = "text";
    root["UserIntent"] = "intent";
    TaskStop taskStop;

    // When
    bool result = taskStop.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStop.userInputText, "text");
    EXPECT_EQ(taskStop.userInputObject, "");
    EXPECT_EQ(taskStop.userIntent, "intent");
    EXPECT_EQ(taskStop.userSpecifiedAgent, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, AllFieldsEmpty1) {
    // Given
    Json::Value root;
    root["UserInputText"] = "";
    root["UserInputObject"] = "";
    root["UserIntent"] = "";
    root["UserSpecifiedAgent"] = "";
    TaskStop taskStop;

    // When
    bool result = taskStop.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStop.userInputText, "");
    EXPECT_EQ(taskStop.userInputObject, "");
    EXPECT_EQ(taskStop.userIntent, "");
    EXPECT_EQ(taskStop.userSpecifiedAgent, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, NoFieldsPresent1) {
    // Given
    Json::Value root;
    TaskStop taskStop;

    // When
    bool result = taskStop.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStop.userInputText, "");
    EXPECT_EQ(taskStop.userInputObject, "");
    EXPECT_EQ(taskStop.userIntent, "");
    EXPECT_EQ(taskStop.userSpecifiedAgent, "");

    // Clean
    GlobalMockObject::verify();
}

