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

TEST_F(TestParse, TaskStatusCodeAndReportPresent) {
    // Given
    Json::Value root;
    root["TaskStatusCode"] = "SUCCESS";
    root["TaskStatusReport"] = "Operation completed successfully";

    // When
    QueryTaskStatus queryTaskStatus;
    bool result = queryTaskStatus.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(queryTaskStatus.taskStatusCode, "SUCCESS");
    EXPECT_EQ(queryTaskStatus.taskStatusReport, "Operation completed successfully");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, TaskStatusCodeMissing) {
    // Given
    Json::Value root;
    root["TaskStatusReport"] = "Operation completed successfully";

    // When
    QueryTaskStatus queryTaskStatus;
    bool result = queryTaskStatus.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(queryTaskStatus.taskStatusCode, "");
    EXPECT_EQ(queryTaskStatus.taskStatusReport, "Operation completed successfully");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, TaskStatusReportMissing) {
    // Given
    Json::Value root;
    root["TaskStatusCode"] = "SUCCESS";

    // When
    QueryTaskStatus queryTaskStatus;
    bool result = queryTaskStatus.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(queryTaskStatus.taskStatusCode, "SUCCESS");
    EXPECT_EQ(queryTaskStatus.taskStatusReport, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, BothFieldsMissing) {
    // Given
    Json::Value root;

    // When
    QueryTaskStatus queryTaskStatus;
    bool result = queryTaskStatus.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(queryTaskStatus.taskStatusCode, "");
    EXPECT_EQ(queryTaskStatus.taskStatusReport, "");

    // Clean
    GlobalMockObject::verify();
}

