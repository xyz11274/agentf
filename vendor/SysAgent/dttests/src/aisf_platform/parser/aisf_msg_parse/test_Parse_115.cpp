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

TEST_F(TestParse, AllFieldsPresent) {
    // Given
    Json::Value root;
    root["UserInputText"] = "Sample Text";
    root["UserInputObject"] = "Sample Object";
    root["UserIntent"] = "Sample Intent";
    root["ImplicitInputFlag"] = true;
    UserInput userInput;

    // When
    bool result = userInput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userInput.userInputText, "Sample Text");
    EXPECT_EQ(userInput.userInputObject, "Sample Object");
    EXPECT_EQ(userInput.userIntent, "Sample Intent");
    EXPECT_EQ(userInput.implicitInputFlag, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, SomeFieldsMissing) {
    // Given
    Json::Value root;
    root["UserInputText"] = "Sample Text";
    root["ImplicitInputFlag"] = false;
    UserInput userInput;

    // When
    bool result = userInput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userInput.userInputText, "Sample Text");
    EXPECT_EQ(userInput.userInputObject, "");
    EXPECT_EQ(userInput.userIntent, "");
    EXPECT_EQ(userInput.implicitInputFlag, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, AllFieldsEmpty) {
    // Given
    Json::Value root;
    root["UserInputText"] = "";
    root["UserInputObject"] = "";
    root["UserIntent"] = "";
    root["ImplicitInputFlag"] = false;
    UserInput userInput;

    // When
    bool result = userInput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userInput.userInputText, "");
    EXPECT_EQ(userInput.userInputObject, "");
    EXPECT_EQ(userInput.userIntent, "");
    EXPECT_EQ(userInput.implicitInputFlag, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, NoFieldsPresent) {
    // Given
    Json::Value root;
    UserInput userInput;

    // When
    bool result = userInput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userInput.userInputText, "");
    EXPECT_EQ(userInput.userInputObject, "");
    EXPECT_EQ(userInput.userIntent, "");
    EXPECT_EQ(userInput.implicitInputFlag, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, InvalidImplicitInputFlag) {
    // Given
    Json::Value root;
    root["UserInputText"] = "Sample Text";
    root["ImplicitInputFlag"] = "not a boolean";
    UserInput userInput;

    // When
    bool result = userInput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userInput.userInputText, "Sample Text");
    EXPECT_EQ(userInput.userInputObject, "");
    EXPECT_EQ(userInput.userIntent, "");
    EXPECT_EQ(userInput.implicitInputFlag, false);

    // Clean
    GlobalMockObject::verify();
}

