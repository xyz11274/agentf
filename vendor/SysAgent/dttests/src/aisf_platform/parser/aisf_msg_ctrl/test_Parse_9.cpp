/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "framework_common/utils/env_util.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace SYSAGENT;

class TestAccessCtrl_Parse : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestAccessCtrl_Parse, ShouldReturnError_WhenInputIsNull) {
    // When
    AccessCtrl accessCtrl;
    string result = accessCtrl.Parse(nullptr, 0);
    // Then
    EXPECT_EQ(result, "input is null");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnError_WhenInputLengthIsZero) {
    // When
    AccessCtrl accessCtrl;
    string result = accessCtrl.Parse("valid json", 0);
    // Then
    EXPECT_EQ(result, "input is zero");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnError_WhenMsgTotalParseFails) {
    // Given
    MOCKER(&MsgAisfTotal::Parse).stubs().will(returnValue(false));
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "UserTaskManagement"}, "Data": {"MessageType": "TaskAssignment"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnUnknownMessageType_WhenHeaderMessageTypeIsNotSupported) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "UnsupportedType"}, "Data": {"MessageType": "SomeType"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnError_WhenUserTaskManagementHasUnknownDataMessageType) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "UserTaskManagement"}, "Data": {"MessageType": "UnknownSubType"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnError_WhenDataExchangeHasUnknownDataMessageType) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "DataExchange"}, "Data": {"MessageType": "UnknownSubType"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnError_WhenLifecycleManagementHasUnknownDataMessageType) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "LifecycleManagement"}, "Data": {"MessageType": "UnknownSubType"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForUserTaskManagement_TaskAssignment) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "UserTaskManagement"}, "Data": {"MessageType": "TaskAssignment"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
    EXPECT_EQ(accessCtrl.eventType, EventType::TASKASSIGNMENT);
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnFailure_WhenTaskAssignmentParseFails) {
    // Given
    // MsgAisfTotal msgTotal{};
    // MOCKER(msgTotal.Parse).stubs().will(returnValue(false));
    MOCKER_CPP(&MsgAisfTotal::Parse, bool (MsgAisfTotal::*)(const char*, size_t)).stubs().will(returnValue(false));

    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "UserTaskManagement"}, "Data": {"MessageType": "TaskAssignment"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForDataExchange_UserInput) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "DataExchange"}, "Data": {"MessageType": "UserInput"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForDataExchange_UserOutput) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "DataExchange"}, "Data": {"MessageType": "UserOutput"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForDataExchange_DataExchange) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "DataExchange"}, "Data": {"MessageType": "DataExchange"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForTaskStatusManagement) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "TaskStatusManagement"}, "Data": {"MessageType": "SomeType"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForLifecycleManagement_LoginOut) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "LifecycleManagement"}, "Data": {"MessageType": "LoginOut"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForLifecycleManagement_LocationInfo) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "LifecycleManagement"}, "Data": {"MessageType": "LocationInfo"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

TEST_F(TestAccessCtrl_Parse, ShouldReturnSuccess_WhenAllFieldsAreValidForLifecycleManagement_UserPortraitConf) {
    // When
    AccessCtrl accessCtrl;
    string jsonStr = R"({"Header": {"MessageType": "LifecycleManagement"}, "Data": {"MessageType": "UserPortraitConf"}})";
    string result = accessCtrl.Parse(jsonStr.c_str(), jsonStr.length());
    // Then
    EXPECT_EQ(result, "MsgTotal error");
}

