///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "model/ModelUtils.h"
//#include "aisf_platform/parser/json_parser.h"
//#include "jsoncpp/value.h"
//#include <regex>
//#include <vector>
//
//class TestParseIntentTasks: public ::testing::Test
//{
//protected:
//   static void SetUpTestCase()
//   {
//   }
//
//   static void TearDownTestCase()
//   {
//   }
//
//   virtual void SetUp()
//   {
//   }
//
//   virtual void TearDown()
//   {
//       GlobalMockObject::verify();
//   }
//};
//
//TEST_F(TestParseIntentTasks, invalidJson) {
//    // Given
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(false));
//
//    // When
//    std::string result = "invalid json";
//    std::string request = "test request";
//    std::string intentPath = "test path";
//    std::vector<IntentTask> intentTasks = parseIntentTasks(result, request, intentPath);
//
//    // Then
//    EXPECT_EQ(intentTasks.size(), 0);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestParseIntentTasks, jsonArrayEmpty) {
//    // Given
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
//    Json::Value jsonReq(Json::arrayValue);
//    MOCKER(static_cast<bool(*)(const Json::Value&, const std::string&, std::string&)>(&JsonParser::GetString)).stubs().will(returnValue(false));
//
//    // When
//    std::string result = "[]";
//    std::string request = "test request";
//    std::string intentPath = "test path";
//    std::vector<IntentTask> intentTasks = parseIntentTasks(result, request, intentPath);
//
//    // Then
//    EXPECT_EQ(intentTasks.size(), 0);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestParseIntentTasks, jsonArrayValid) {
//    // Given
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
//    Json::Value jsonReq(Json::arrayValue);
//    Json::Value item(Json::objectValue);
//    item["agent_name"] = "agent1";
//    item["context"] = "context1";
//    item["APP"] = "app1";
//    jsonReq.append(item);
//    MOCKER(static_cast<bool(*)(const Json::Value&, const std::string&, std::string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));
//
//    // When
//    std::string result = "[{\"agent_name\": \"agent1\", \"context\": \"context1\", \"APP\": \"app1\"}]";
//    std::string request = "test request";
//    std::string intentPath = "test path";
//    std::vector<IntentTask> intentTasks = parseIntentTasks(result, request, intentPath);
//
//    // Then
//    EXPECT_EQ(intentTasks.size(), 1);
//    EXPECT_EQ(intentTasks[0].agentName, "agent1");
//    EXPECT_EQ(intentTasks[0].requestContent, request);
//    EXPECT_EQ(intentTasks[0].APP, "app1");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestParseIntentTasks, jsonObjectValid) {
//    // Given
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
//    Json::Value jsonReq(Json::objectValue);
//    jsonReq["agent_name"] = "agent1";
//    jsonReq["context"] = "context1";
//    jsonReq["APP"] = "app1";
//    MOCKER(static_cast<bool(*)(const Json::Value&, const std::string&, std::string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));
//
//    // When
//    std::string result = "{\"agent_name\": \"agent1\", \"context\": \"context1\", \"APP\": \"app1\"}";
//    std::string request = "test request";
//    std::string intentPath = "test path";
//    std::vector<IntentTask> intentTasks = parseIntentTasks(result, request, intentPath);
//
//    // Then
//    EXPECT_EQ(intentTasks.size(), 1);
//    EXPECT_EQ(intentTasks[0].agentName, "agent1");
//    EXPECT_EQ(intentTasks[0].requestContent, request);
//    EXPECT_EQ(intentTasks[0].APP, "app1");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestParseIntentTasks, jsonObjectMissingFields) {
//    // Given
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
//    Json::Value jsonReq(Json::objectValue);
//    jsonReq["agent_name"] = "agent1";
//    MOCKER(static_cast<bool(*)(const Json::Value&, const std::string&, std::string&)>(&JsonParser::GetString)).stubs()
//        .with(anything(), eq("agent_name"), anything()).will(returnValue(true))
//        .with(anything(), eq("context"), anything()).will(returnValue(false));
//
//    // When
//    std::string result = "{\"agent_name\": \"agent1\"}";
//    std::string request = "test request";
//    std::string intentPath = "test path";
//    std::vector<IntentTask> intentTasks = parseIntentTasks(result, request, intentPath);
//
//    // Then
//    EXPECT_EQ(intentTasks.size(), 0);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
