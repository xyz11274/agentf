/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "model/ModelUtils.h"
#include "aisf_platform/parser/json_parser.h"
//#include "jsoncpp/value.h"
#include <regex>
#include <vector>

class TestParseBertAnswer: public ::testing::Test
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

TEST_F(TestParseBertAnswer, emptyJsonArray) {
    // Given
    string answer = "[]";
//    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(false));
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().with(smirror(answer.c_str()), ieq(answer.size())).will(returnValue(true));

    // When
    vector<BertReault> results = parseBertAnswer(answer);

    // Then
    EXPECT_EQ(results.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParseBertAnswer, validJsonArray) {
    // Given
    std::string answer = "[{\"input\":\"我想看刘德华的喜剧电影\",\"intent\":\"INTENT_VIDEO_SEARCH_PLAY\",\n"
                         "\"slot_prediction\":\"O O O O O O O B-type I-type B-category I-category\",\n"
                         "\"slot\":\"我想看刘德华的[喜:B-type][剧:I-type][电:B-category][影:I-category]\",\n"
                         "\"confidence\":[\"INTENT_VIDEO_SEARCH_PLAY:99.9%\",\"INTENT_PLAY_SPEED:0.0%\",\n"
                         "\"INTENT_PLAY_CONTINUE:0.0%\",\"INTENT_AVCALL_EDIT_CONTACT:0.0%\",\"INTENT_PLAY_BACKWORD:0.0%\"]}]";

    // When
    vector<BertReault> results = parseBertAnswer(answer);


    // Then
    EXPECT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].intent, "INTENT_VIDEO_SEARCH_PLAY");
    EXPECT_EQ(results[0].params.size(), 2);
    EXPECT_EQ(results[0].params["type"], "喜剧");
    EXPECT_EQ(results[0].params["category"], "电影");

    // Clean
    GlobalMockObject::verify();
}
//
//TEST_F(TestParseBertAnswer, invalidJsonArray) {
//    // Given
//    string answer = "invalid_json";
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().with(smirror(answer.c_str()), ieq(answer.size())).will(returnValue(false));
//
//    // When
//    vector<BertReault> results = parseBertAnswer(answer);
//
//    // Then
//    EXPECT_EQ(results.size(), 0);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestParseBertAnswer, missingIntent) {
//    // Given
//    string answer = R"([{"slot": "[B-name:Alice]-[I-name:Bob]"}])";
//    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
//        .with(_, smirror("intent"), _).will(returnValue(false))
//        .with(_, smirror("slot"), _).will(sideEffect([](const Json::Value& node, const string& member, string& value) { value = "[B-name:Alice]-[I-name:Bob]"; return true; }));
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().with(smirror(answer.c_str()), ieq(answer.size())).will(returnValue(true));
//
//    // When
//    vector<BertReault> results = parseBertAnswer(answer);
//
//    // Then
//    EXPECT_EQ(results.size(), 1);
//    EXPECT_EQ(results[0].intent, "");
//    EXPECT_EQ(results[0].params.size(), 1);
//    EXPECT_EQ(results[0].params["name"], "AliceBob");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestParseBertAnswer, missingSlot) {
//    // Given
//    string answer = R"([{"intent": "greet"}])";
//    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
//        .with(_, smirror("intent"), _).will(sideEffect([](const Json::Value& node, const string& member, string& value) { value = "greet"; return true; }))
//        .with(_, smirror("slot"), _).will(returnValue(false));
//    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().with(smirror(answer.c_str()), ieq(answer.size())).will(returnValue(true));
//
//    // When
//    vector<BertReault> results = parseBertAnswer(answer);
//
//    // Then
//    EXPECT_EQ(results.size(), 1);
//    EXPECT_EQ(results[0].intent, "greet");
//    EXPECT_EQ(results[0].params.size(), 0);
//
//    // Clean
//    GlobalMockObject::verify();
//}

