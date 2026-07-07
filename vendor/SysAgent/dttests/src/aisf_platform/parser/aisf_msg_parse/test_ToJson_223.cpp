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

TEST_F(TestToJson, ut_with_empty_strings3) {
    // Given
    TaskStatusReport taskStatusReport;
    Json::Value jsonObj;

    // When
    taskStatusReport.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["TaskStatusCode"].asString(), "");
    EXPECT_EQ(jsonObj["TaskStatusReport"].asString(), "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestToJson, ut_with_non_empty_strings3) {
    // Given
    TaskStatusReport taskStatusReport;
    taskStatusReport.taskStatusCode = "200";
    taskStatusReport.taskStatusReport = "Success";
    Json::Value jsonObj;

    // When
    taskStatusReport.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["TaskStatusCode"].asString(), "200");
    EXPECT_EQ(jsonObj["TaskStatusReport"].asString(), "Success");

    // Clean
    GlobalMockObject::verify();
}

