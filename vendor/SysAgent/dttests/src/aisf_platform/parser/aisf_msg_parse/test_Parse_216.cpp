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

TEST_F(TestParse, ut_missingTaskStatusCode) {
    // Given
    Json::Value root;
    root["TaskStatusReport"] = "ReportContent";
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().with(any(), eq("TaskStatusCode"), any()).will(returnValue(false)).then(returnValue(true));
    MOCKER(&JsonParser::GetString).stubs().will(returnValue(false)).then(returnValue(true));

    // When
    TaskStatusReport taskStatusReport;
    bool result = taskStatusReport.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStatusReport.taskStatusCode, "");
    EXPECT_EQ(taskStatusReport.taskStatusReport, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_missingTaskStatusReport) {
    // Given
    Json::Value root;
    root["TaskStatusCode"] = "200";
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().with(any(), eq("TaskStatusCode"), any()).will(returnValue(true)).then(returnValue(false));
    MOCKER(&JsonParser::GetString).stubs().will(returnValue(true)).then(returnValue(false));

    // When
    TaskStatusReport taskStatusReport;
    bool result = taskStatusReport.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStatusReport.taskStatusCode, "");
    EXPECT_EQ(taskStatusReport.taskStatusReport, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_allFieldsPresent) {
    // Given
    Json::Value root;
    root["TaskStatusCode"] = "200";
    root["TaskStatusReport"] = "ReportContent";
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().with(any(), eq("TaskStatusCode"), any()).will(returnValue(true)).then(returnValue(true));
    MOCKER(&JsonParser::GetString).stubs().will(returnValue(true)).then(returnValue(true));
    // When
    TaskStatusReport taskStatusReport;
    bool result = taskStatusReport.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStatusReport.taskStatusCode, "");
    EXPECT_EQ(taskStatusReport.taskStatusReport, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_allFieldsMissing) {
    // Given
    Json::Value root;
    // MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().with(any(), eq("TaskStatusCode"), any()).will(returnValue(false)).then(returnValue(false));
    MOCKER(&JsonParser::GetString).stubs().will(returnValue(false)).then(returnValue(false));

    // When
    TaskStatusReport taskStatusReport;
    bool result = taskStatusReport.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(taskStatusReport.taskStatusCode, "");
    EXPECT_EQ(taskStatusReport.taskStatusReport, "");

    // Clean
    GlobalMockObject::verify();
}

