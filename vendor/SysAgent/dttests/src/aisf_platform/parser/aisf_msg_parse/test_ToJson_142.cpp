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

TEST_F(TestToJson, AllFieldsEmpty) {
    // Given
    TaskStop taskStop;
    taskStop.userInputText = "";
    taskStop.userInputObject = "";
    taskStop.userIntent = "";
    taskStop.userSpecifiedAgent = "";
    Json::Value jsonObj;

    // When
    taskStop.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "TaskStop");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "");
    EXPECT_EQ(jsonObj["UserSpecifiedAgent"].asString(), "");

}

TEST_F(TestToJson, AllFieldsFilled) {
    // Given
    TaskStop taskStop;
    taskStop.userInputText = "Text";
    taskStop.userInputObject = "Object";
    taskStop.userIntent = "Intent";
    taskStop.userSpecifiedAgent = "Agent";
    Json::Value jsonObj;

    // When
    taskStop.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "TaskStop");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "Text");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "Object");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "Intent");
    EXPECT_EQ(jsonObj["UserSpecifiedAgent"].asString(), "Agent");

}

TEST_F(TestToJson, SomeFieldsFilled) {
    // Given
    TaskStop taskStop;
    taskStop.userInputText = "Text";
    taskStop.userInputObject = "";
    taskStop.userIntent = "Intent";
    taskStop.userSpecifiedAgent = "";
    Json::Value jsonObj;

    // When
    taskStop.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "TaskStop");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "Text");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "Intent");
    EXPECT_EQ(jsonObj["UserSpecifiedAgent"].asString(), "");

}

