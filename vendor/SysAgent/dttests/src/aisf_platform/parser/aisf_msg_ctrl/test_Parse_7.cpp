/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace SYSAGENT;

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

TEST_F(TestParse, inputIsNull) {
    // Given
    AccessCtrl accessCtrl;

    // When
    string result = accessCtrl.Parse(nullptr, 10);

    // Then
    EXPECT_EQ(result, "input is null");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, inputLengthIsZero) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "";

    // When
    string result = accessCtrl.Parse(input, 0);

    // Then
    EXPECT_EQ(result, "input is zero");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, msgTotalParseFails) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"UserTaskManagement\"}}";
    size_t length = strlen(input);
    MOCKER_CPP(&MsgAisfTotal::Parse, bool (MsgAisfTotal::*)(const char*, size_t)).stubs().will(returnValue(false));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "MsgTotal error");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, userTaskManagementTaskAssignment) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"UserTaskManagement\"},\"Data\":{\"MessageType\":\"TaskAssignment\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<TaskAssignment, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<TaskAssignment, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, userTaskManagementTaskStop) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"UserTaskManagement\"},\"Data\":{\"MessageType\":\"TaskStop\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<TaskStop, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<TaskStop, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, userTaskManagementUnknown) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"UserTaskManagement\"},\"Data\":{\"MessageType\":\"Unknown\"}}";
    size_t length = strlen(input);
    MOCKER_CPP(&MsgAisfTotal::Parse, bool (MsgAisfTotal::*)(const char*, size_t)).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, dataExchangeUserInput) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"DataExchange\"},\"Data\":{\"MessageType\":\"UserInput\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<UserInput, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);
    
    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<UserInput, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, dataExchangeUserOutput) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"DataExchange\"},\"Data\":{\"MessageType\":\"UserOutput\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<UserOutput, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<UserOutput, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, dataExchangeDataExchange) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"DataExchange\"},\"Data\":{\"MessageType\":\"DataExchange\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<DataExchange, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<DataExchange, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, dataExchangeUnknown) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"DataExchange\"},\"Data\":{\"MessageType\":\"Unknown\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, taskStatusManagement) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"TaskStatusManagement\"},\"Data\":{\"MessageType\":\"QueryTaskStatus\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<TaskStatusManagement, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<TaskStatusManagement, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, lifecycleManagementLoginOut) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"LifecycleManagement\"},\"Data\":{\"MessageType\":\"LoginOut\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<LoginOut, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<LoginOut, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, lifecycleManagementLocationInfo) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"LifecycleManagement\"},\"Data\":{\"MessageType\":\"LocationInfo\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<LocationInfo, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<LocationInfo, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, lifecycleManagementUnknown) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"LifecycleManagement\"},\"Data\":{\"MessageType\":\"Unknown\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result,"unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, unknownMessageType) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"Unknown\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, parseFailure) {
    // Given
    AccessCtrl accessCtrl;
    const char* input = "{\"Header\":{\"MessageType\":\"UserTaskManagement\"},\"Data\":{\"MessageType\":\"TaskAssignment\"}}";
    size_t length = strlen(input);

    // 修正函数指针类型：将引用改为指针
    using MsgAisfTotalParseFunc = bool (MsgAisfTotal::*)(const char*, size_t);
    using MessageAisfParseFunc = bool (MessageAisf<TaskAssignment, MsgPayload>::*)(const string&, const MsgAisfHeader&, const Json::Value*, const Json::Value*);

    MOCKER_CPP(&MsgAisfTotal::Parse, MsgAisfTotalParseFunc).stubs().will(returnValue(true));
    MOCKER_CPP((&MessageAisf<TaskAssignment, MsgPayload>::Parse), MessageAisfParseFunc).stubs().will(returnValue(false));

    // When
    string result = accessCtrl.Parse(input, length);

    // Then
    EXPECT_EQ(result, "unknown:");

    // Clean
    GlobalMockObject::verify();
}

