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

class TestAccessCtrlSetClientExField : public ::testing::Test {
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

TEST_F(TestAccessCtrlSetClientExField, ShouldSetClientExFieldInTaskAssignment_WhenEventTypeIsTaskAssignment) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.taskAssignment.header.clientExField, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetClientExFieldInUserInput_WhenEventTypeIsUserInput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USERINPUT;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.userInput.header.clientExField, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetClientExFieldInDataExchange_WhenEventTypeIsDataExchange) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::DATAEXCHANGE;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.dataExchange.header.clientExField, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetClientExFieldInTaskStatusManagement_WhenEventTypeIsTaskStatusManagement) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.taskStatusManagement.header.clientExField, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetClientExFieldInUserOutput_WhenEventTypeIsUserOutput) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USEROUTPUT;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.userOutput.header.clientExField, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetClientExFieldInTaskStop_WhenEventTypeIsTaskStop) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTOP;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.taskStop.header.clientExField, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetClientExFieldInLoginOut_WhenEventTypeIsLoginOut) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOGINOUT;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.loginOut.header.clientExField, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetSenderInLocationInfo_WhenEventTypeIsLocation) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::LOCATION;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.locationInfo.header.sender, "test_client_ex_field");
}

TEST_F(TestAccessCtrlSetClientExField, ShouldSetSenderInUserPortrait_WhenEventTypeIsUserPortraitConf) {
    // Given
    AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USERPORTRAITCONF;
    // When
    accessCtrl.setClientExField("test_client_ex_field");
    // Then
    EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
    EXPECT_EQ(accessCtrl.userPortrait.header.sender, "test_client_ex_field");
}

// TEST_F(TestAccessCtrlSetClientExField, ShouldLogWarning_WhenEventTypeIsUnknown) {
//     // Given
//     AccessCtrl accessCtrl;
//     accessCtrl.eventType = SYSAGENT::EventType::UNKNOWN;
//     // When
//     accessCtrl.setClientExField("test_client_ex_field");
//     // Then
//     EXPECT_EQ(accessCtrl.header.clientExField, "test_client_ex_field");
// }

