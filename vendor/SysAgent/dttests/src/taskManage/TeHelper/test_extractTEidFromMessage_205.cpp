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

class TestExtractTEidFromMessage: public ::testing::Test
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

TEST_F(TestExtractTEidFromMessage, NullPointer) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = nullptr;

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "");

}

TEST_F(TestExtractTEidFromMessage, TaskAssignment) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    data->taskAssignment.header.teamEnvironmentID = "TE123";

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "TE123");

}

TEST_F(TestExtractTEidFromMessage, TaskStop) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKSTOP;
    data->taskStop.header.teamEnvironmentID = "TE456";

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "TE456");

}

TEST_F(TestExtractTEidFromMessage, UserInput) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::USERINPUT;
    data->userInput.header.teamEnvironmentID = "TE789";

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "TE789");

}

TEST_F(TestExtractTEidFromMessage, UserOutput) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::USEROUTPUT;
    data->userOutput.header.teamEnvironmentID = "TE101";

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "TE101");

}

TEST_F(TestExtractTEidFromMessage, DataExchange) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    data->dataExchange.header.teamEnvironmentID = "TE202";

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "TE202");

}

TEST_F(TestExtractTEidFromMessage, TaskStatusManagement) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    data->taskStatusManagement.header.teamEnvironmentID = "TE303";

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "TE303");

}

TEST_F(TestExtractTEidFromMessage, LoginOut) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = SYSAGENT::EventType::LOGINOUT;
    data->loginOut.header.teamEnvironmentID = "TE404";

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "TE404");

}

TEST_F(TestExtractTEidFromMessage, UnhandledEventType) {
    // Given
    auto data = std::make_shared<SYSAGENT::AccessCtrl>();
    data->eventType = static_cast<SYSAGENT::EventType>(999);

    // When
    std::string result = TEHelper::extractTEidFromMessage(data);

    // Then
    EXPECT_EQ(result, "");

}

