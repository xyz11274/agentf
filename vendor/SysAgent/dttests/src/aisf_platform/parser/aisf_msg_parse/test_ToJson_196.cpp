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

TEST_F(TestToJson, ToJsonWithAllFields) {
    // Given
    TaskStatusManagement taskStatusManagement;
    taskStatusManagement.messageType = "Request";
    taskStatusManagement.taskStatusCode = 200;
    taskStatusManagement.taskStatusReport = "Success";

    Json::Value jsonObj;

    // When
    taskStatusManagement.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "Request");
    EXPECT_EQ(jsonObj["TaskStatusCode"].asInt(), 200);
    EXPECT_EQ(jsonObj["TaskStatusReport"].asString(), "Success");

}

TEST_F(TestToJson, ToJsonWithoutOptionalFields) {
    // Given
    TaskStatusManagement taskStatusManagement;
    taskStatusManagement.messageType = "Request";

    Json::Value jsonObj;

    // When
    taskStatusManagement.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "Request");
    EXPECT_EQ(jsonObj["TaskStatusCode"].isInt(), true);
    EXPECT_EQ(jsonObj["TaskStatusReport"].isString(), true);

}

