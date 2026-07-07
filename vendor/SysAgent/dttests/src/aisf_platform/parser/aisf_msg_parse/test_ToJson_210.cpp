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

TEST_F(TestToJson, ut_with_empty_strings) {
    // Given
    QueryTaskStatus queryTaskStatus;
    Json::Value jsonObj;

    // When
    queryTaskStatus.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["TaskStatusCode"].asString(), "");
    EXPECT_EQ(jsonObj["TaskStatusReport"].asString(), "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestToJson, ut_with_non_empty_strings) {
    // Given
    QueryTaskStatus queryTaskStatus;
    queryTaskStatus.taskStatusCode = "Completed";
    queryTaskStatus.taskStatusReport = "Task completed successfully.";
    Json::Value jsonObj;

    // When
    queryTaskStatus.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["TaskStatusCode"].asString(), "Completed");
    EXPECT_EQ(jsonObj["TaskStatusReport"].asString(), "Task completed successfully.");

    // Clean
    GlobalMockObject::verify();
}

