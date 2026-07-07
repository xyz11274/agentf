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

class TestExtractTaskId: public ::testing::Test
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

TEST_F(TestExtractTaskId, nullAccessCtrl) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "");

}

TEST_F(TestExtractTaskId, taskAssignment) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    accessCtrl->taskAssignment.header.taskID = "task123";

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "task123");

}

TEST_F(TestExtractTaskId, taskStop) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTOP;
    accessCtrl->taskStop.header.taskID = "task456";

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "task456");

}

TEST_F(TestExtractTaskId, userInput) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
    accessCtrl->userInput.header.taskID = "task789";

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "task789");

}

TEST_F(TestExtractTaskId, userOutput) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
    accessCtrl->userOutput.header.taskID = "task101";

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "task101");

}

TEST_F(TestExtractTaskId, dataExchange) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    accessCtrl->dataExchange.header.taskID = "task202";

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "task202");

}

TEST_F(TestExtractTaskId, taskStatusManagement) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    accessCtrl->taskStatusManagement.header.taskID = "task303";

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "task303");

}

TEST_F(TestExtractTaskId, loginOut) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = SYSAGENT::EventType::LOGINOUT;
    accessCtrl->loginOut.header.taskID = "task404";

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "task404");

}

TEST_F(TestExtractTaskId, unknownEventType) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->eventType = static_cast<SYSAGENT::EventType>(999); // Assuming 999 is an unknown event type

    // When
    std::string result = TEHelper::extractTaskId(accessCtrl);

    // Then
    EXPECT_EQ(result, "");

}

