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

TEST_F(TestToJson, AllFieldsSet2) {
    // Given
    UserInput userInput;
    userInput.userInputText = "Sample Text";
    userInput.userInputObject = "Sample Object";
    userInput.userIntent = "Sample Intent";
    userInput.implicitInputFlag = true;
    Json::Value jsonObj;

    // When
    userInput.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "UserInput");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "Sample Text");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "Sample Object");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "Sample Intent");
    EXPECT_EQ(jsonObj["ImplicitInputFlag"].asBool(), true);

}

TEST_F(TestToJson, OptionalFieldsEmpty2) {
    // Given
    UserInput userInput;
    Json::Value jsonObj;

    // When
    userInput.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "UserInput");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "");
    EXPECT_EQ(jsonObj["ImplicitInputFlag"].asBool(), false);

}

TEST_F(TestToJson, ImplicitInputFlagFalse) {
    // Given
    UserInput userInput;
    userInput.userInputText = "Sample Text";
    userInput.userInputObject = "Sample Object";
    userInput.userIntent = "Sample Intent";
    userInput.implicitInputFlag = false;
    Json::Value jsonObj;

    // When
    userInput.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "UserInput");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "Sample Text");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "Sample Object");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "Sample Intent");
    EXPECT_EQ(jsonObj["ImplicitInputFlag"].asBool(), false);

}

