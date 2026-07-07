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

TEST_F(TestToJson, AllFieldsSet) {
    // Given
    TaskAssignment taskAssignment;
    taskAssignment.userInputText = "Sample Text";
    taskAssignment.userInputObject = "Sample Object";
    taskAssignment.userIntent = "Sample Intent";
    taskAssignment.userSpecifiedAgent = "Sample Agent";
    taskAssignment.APPName = "Sample App";
    Json::Value jsonObj;

    // When
    taskAssignment.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "TaskAssignment");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "Sample Text");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "Sample Object");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "Sample Intent");
    EXPECT_EQ(jsonObj["UserSpecifiedAgent"].asString(), "Sample Agent");
    EXPECT_EQ(jsonObj["APPName"].asString(), "Sample App");

}

TEST_F(TestToJson, OptionalFieldsEmpty) {
    // Given
    TaskAssignment taskAssignment;
    Json::Value jsonObj;

    // When
    taskAssignment.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "TaskAssignment");
    EXPECT_EQ(jsonObj["UserInputText"].asString(), "");
    EXPECT_EQ(jsonObj["UserInputObject"].asString(), "");
    EXPECT_EQ(jsonObj["UserIntent"].asString(), "");
    EXPECT_EQ(jsonObj["UserSpecifiedAgent"].asString(), "");
    EXPECT_EQ(jsonObj["APPName"].asString(), "");

}

