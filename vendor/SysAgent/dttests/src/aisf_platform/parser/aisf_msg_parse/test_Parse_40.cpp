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

TEST_F(TestParse, AllFieldsPresent2) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["TeamEnvironmentID"] = "EnvID";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";
    root["AcmSessionId"] = "ACM";
    root["TaskID"] = "Task";
    root["EventTimeStamp"] = "Time";
    root["ClientExField"] = "Field";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs().will(returnValue(true));

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, MessageTypeMissing) {
    // Given
    Json::Value root;
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(false))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, SenderMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(false))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, false);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ReceiverMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["UserName"] = "User";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, UserNameMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, DeviceIdMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次
        
    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, DeviceCategoryMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["DeviceId"] = "Device";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, UUIDMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["ClientSessionId"] = "Session";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ClientSessionIdMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["EventTimeStamp"] = "Time";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, EventTimeStampMissing) {
    // Given
    Json::Value root;
    root["MessageType"] = "Type";
    root["Sender"] = "Sender";
    root["Receiver"] = "Receiver";
    root["UserName"] = "User";
    root["DeviceId"] = "Device";
    root["DeviceCategory"] = "Category";
    root["UUID"] = "UUID";
    root["ClientSessionId"] = "Session";

    MOCKER(static_cast<bool(*)(const Json::Value&, const string&, string&)>(&JsonParser::GetString)).stubs()
        .then(returnValue(true))   // 第1次
        .then(returnValue(true))    // 第2次
        .then(returnValue(true))    // 第3次
        .then(returnValue(true))    // 第4次
        .then(returnValue(true))    // 第5次
        .then(returnValue(true))    // 第6次
        .then(returnValue(true))    // 第7次
        .then(returnValue(true))    // 第8次
        .then(returnValue(true));   // 第9次

    MsgAisfHeader msgAisfHeader;

    // When
    bool result = msgAisfHeader.Parse(root);

    // Then
    EXPECT_EQ(result, true);

    // Clean
    GlobalMockObject::verify();
}

