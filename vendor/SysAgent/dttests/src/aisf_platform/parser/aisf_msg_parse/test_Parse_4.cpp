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

TEST_F(TestParse, RawStringParseError) {
    // Given
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(false));

    // When
    MsgAisfTotal msgAisfTotal;
    bool ret = msgAisfTotal.Parse("{\"Header\":{}, \"Data\":{}, \"Payload\":{}}", 38);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, HeaderGetError) {
    // Given
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
    // MOCKER(static_cast<const Json::Value*(*)(const Json::Value&, const string&)>(&JsonParser::GetNode)).stubs().will(returnValue(nullptr));
    MOCKER(&JsonParser::GetNode).stubs().will(returnValue(static_cast<const Json::Value*>(nullptr)));

    // When
    MsgAisfTotal msgAisfTotal;
    bool ret = msgAisfTotal.Parse("{\"Header\":{}, \"Data\":{}, \"Payload\":{}}", 38);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, HeaderParseError) {
    // Given
    Json::Value rootJson;
    rootJson["Header"] = Json::Value();
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
    // MOCKER(static_cast<const Json::Value*(*)(const Json::Value&, const string&)>(&JsonParser::GetNode)).stubs().will(returnValue(&rootJson["Header"]));
    MOCKER(&JsonParser::GetNode).stubs().will(returnValue(static_cast<const Json::Value*>(&rootJson["Header"])));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(false));

    // When
    MsgAisfTotal msgAisfTotal;
    bool ret = msgAisfTotal.Parse("{\"Header\":{}, \"Data\":{}, \"Payload\":{}}", 38);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, DataGetError) {
    // Given
    Json::Value rootJson;
    rootJson["Header"] = Json::Value();
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
    // MOCKER(static_cast<const Json::Value*(*)(const Json::Value&, const string&)>(&JsonParser::GetNode))
    //     .stubs()
    //     .then(returnValue(&rootJson["Header"]))  // 第一次调用返回Header
    //     .then(returnValue(nullptr));             // 第二次调用返回nullptr
    MOCKER(&JsonParser::GetNode).stubs().then(returnValue(static_cast<const Json::Value*>(&rootJson["Header"]))).then(returnValue(static_cast<const Json::Value*>(nullptr)));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));

    // When
    MsgAisfTotal msgAisfTotal;
    bool ret = msgAisfTotal.Parse("{\"Header\":{}, \"Data\":{}, \"Payload\":{}}", 38);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, MessageTypeGetError) {
    // Given
    Json::Value rootJson;
    rootJson["Header"] = Json::Value();
    rootJson["Data"] = Json::Value();
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
    // MOCKER(static_cast<const Json::Value*(*)(const Json::Value&, const string&)>(&JsonParser::GetNode))
    //     .stubs()
    //     .then(returnValue(&rootJson["Header"]))
    //     .then(returnValue(&rootJson["Data"]));
    MOCKER(&JsonParser::GetNode).stubs()
        .then(returnValue(static_cast<const Json::Value*>(&rootJson["Header"])))
        .then(returnValue(static_cast<const Json::Value*>(&rootJson["Data"])));

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))
        .then(returnValue(false));

    // When
    MsgAisfTotal msgAisfTotal;
    bool ret = msgAisfTotal.Parse("{\"Header\":{}, \"Data\":{}, \"Payload\":{}}", 38);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, Success) {
    // Given
    Json::Value rootJson;
    rootJson["Header"] = Json::Value();
    rootJson["Data"] = Json::Value();
    rootJson["Payload"] = Json::Value();
    MOCKER(static_cast<bool(*)(const char*, size_t, Json::Value&)>(&JsonParser::Parse)).stubs().will(returnValue(true));
    // MOCKER(static_cast<const Json::Value*(*)(const Json::Value&, const string&)>(&JsonParser::GetNode))
    //     .stubs()
    //     .then(returnValue(&rootJson["Header"]))
    //     .then(returnValue(&rootJson["Data"]))
    //     .then(returnValue(&rootJson["Payload"]));    
    MOCKER(&JsonParser::GetNode).stubs()
        .then(returnValue(static_cast<const Json::Value*>(&rootJson["Header"])))
        .then(returnValue(static_cast<const Json::Value*>(&rootJson["Data"])))
        .then(returnValue(static_cast<const Json::Value*>(&rootJson["Payload"])));
    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString))
        .stubs()
        .then(returnValue(true))
        .then(returnValue(true));

    // When
    MsgAisfTotal msgAisfTotal;
    bool ret = msgAisfTotal.Parse("{\"Header\":{}, \"Data\":{}, \"Payload\":{}}", 38);

    // Then
    EXPECT_EQ(ret, true);

    // Clean
    GlobalMockObject::verify();
}

