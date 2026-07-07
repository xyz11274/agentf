/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/TeHelper.h"
#include "aisf_platform/common_define.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphManager.h"
#include "graphManager/GraphUtils.h"
#include "mm_customedreact_llm_component.h"
#include "queue/QueueManager.h"
#include <utility>

class TestExtractUserInfo: public ::testing::Test
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

TEST_F(TestExtractUserInfo, TaskAssignment) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    data->taskAssignment.header.userName = "Alice";
    data->taskAssignment.header.deviceId = "Device1";
    data->taskAssignment.header.deviceCategory = "Category1";
    data->taskAssignment.header.uuid = "UUID1";
    data->taskAssignment.header.clientSessionId = "Session1";
    data->taskAssignment.header.acmSessionId = "ACM1";

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "Alice");
    EXPECT_EQ(userInfo.deviceId, "Device1");
    EXPECT_EQ(userInfo.deviceCategory, "Category1");
    EXPECT_EQ(userInfo.uuid, "UUID1");
    EXPECT_EQ(userInfo.clientSessionId, "Session1");
    EXPECT_EQ(userInfo.acmSessionId, "ACM1");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractUserInfo, TaskStop) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKSTOP;
    data->taskStop.header.userName = "Bob";
    data->taskStop.header.deviceId = "Device2";
    data->taskStop.header.deviceCategory = "Category2";
    data->taskStop.header.uuid = "UUID2";
    data->taskStop.header.clientSessionId = "Session2";
    data->taskStop.header.acmSessionId = "ACM2";

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "Bob");
    EXPECT_EQ(userInfo.deviceId, "Device2");
    EXPECT_EQ(userInfo.deviceCategory, "Category2");
    EXPECT_EQ(userInfo.uuid, "UUID2");
    EXPECT_EQ(userInfo.clientSessionId, "Session2");
    EXPECT_EQ(userInfo.acmSessionId, "ACM2");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractUserInfo, UserInput) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::USERINPUT;
    data->userInput.header.userName = "Charlie";
    data->userInput.header.deviceId = "Device3";
    data->userInput.header.deviceCategory = "Category3";
    data->userInput.header.uuid = "UUID3";
    data->userInput.header.clientSessionId = "Session3";
    data->userInput.header.acmSessionId = "ACM3";

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "Charlie");
    EXPECT_EQ(userInfo.deviceId, "Device3");
    EXPECT_EQ(userInfo.deviceCategory, "Category3");
    EXPECT_EQ(userInfo.uuid, "UUID3");
    EXPECT_EQ(userInfo.clientSessionId, "Session3");
    EXPECT_EQ(userInfo.acmSessionId, "ACM3");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractUserInfo, DataExchange) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    data->dataExchange.header.userName = "David";
    data->dataExchange.header.deviceId = "Device4";
    data->dataExchange.header.deviceCategory = "Category4";
    data->dataExchange.header.uuid = "UUID4";
    data->dataExchange.header.clientSessionId = "Session4";
    data->dataExchange.header.acmSessionId = "ACM4";

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "David");
    EXPECT_EQ(userInfo.deviceId, "Device4");
    EXPECT_EQ(userInfo.deviceCategory, "Category4");
    EXPECT_EQ(userInfo.uuid, "UUID4");
    EXPECT_EQ(userInfo.clientSessionId, "Session4");
    EXPECT_EQ(userInfo.acmSessionId, "ACM4");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractUserInfo, TaskStatusManagement) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    data->taskStatusManagement.header.userName = "Eve";
    data->taskStatusManagement.header.deviceId = "Device5";
    data->taskStatusManagement.header.deviceCategory = "Category5";
    data->taskStatusManagement.header.uuid = "UUID5";
    data->taskStatusManagement.header.clientSessionId = "Session5";
    data->taskStatusManagement.header.acmSessionId = "ACM5";

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "Eve");
    EXPECT_EQ(userInfo.deviceId, "Device5");
    EXPECT_EQ(userInfo.deviceCategory, "Category5");
    EXPECT_EQ(userInfo.uuid, "UUID5");
    EXPECT_EQ(userInfo.clientSessionId, "Session5");
    EXPECT_EQ(userInfo.acmSessionId, "ACM5");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractUserInfo, UserOutput) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::USEROUTPUT;
    data->userOutput.header.userName = "Frank";
    data->userOutput.header.deviceId = "Device6";
    data->userOutput.header.deviceCategory = "Category6";
    data->userOutput.header.uuid = "UUID6";
    data->userOutput.header.clientSessionId = "Session6";
    data->userOutput.header.acmSessionId = "ACM6";

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "Frank");
    EXPECT_EQ(userInfo.deviceId, "Device6");
    EXPECT_EQ(userInfo.deviceCategory, "Category6");
    EXPECT_EQ(userInfo.uuid, "UUID6");
    EXPECT_EQ(userInfo.clientSessionId, "Session6");
    EXPECT_EQ(userInfo.acmSessionId, "ACM6");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractUserInfo, LoginOut) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::LOGINOUT;
    data->loginOut.header.userName = "Grace";
    data->loginOut.header.deviceId = "Device7";
    data->loginOut.header.deviceCategory = "Category7";
    data->loginOut.header.uuid = "UUID7";
    data->loginOut.header.clientSessionId = "Session7";
    data->loginOut.header.acmSessionId = "ACM7";

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "Grace");
    EXPECT_EQ(userInfo.deviceId, "Device7");
    EXPECT_EQ(userInfo.deviceCategory, "Category7");
    EXPECT_EQ(userInfo.uuid, "UUID7");
    EXPECT_EQ(userInfo.clientSessionId, "Session7");
    EXPECT_EQ(userInfo.acmSessionId, "ACM7");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestExtractUserInfo, UnknownEventType) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = static_cast<SYSAGENT::EventType>(999); // Assuming 999 is an unknown event type

    // When
    UserInfo userInfo = TEHelper::extractUserInfo(data);

    // Then
    EXPECT_EQ(userInfo.userName, "Unknown User");
    EXPECT_EQ(userInfo.deviceId, "");
    EXPECT_EQ(userInfo.deviceCategory, "");
    EXPECT_EQ(userInfo.uuid, "");
    EXPECT_EQ(userInfo.clientSessionId, "");
    EXPECT_EQ(userInfo.acmSessionId, "");

    // Clean
    GlobalMockObject::verify();
}

