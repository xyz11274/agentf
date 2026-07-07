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

class TestSaveTeamEnvID: public ::testing::Test
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

TEST_F(TestSaveTeamEnvID, TaskAssignment) {
    // Given
    auto accCtr = std::make_shared<SYSAGENT::AccessCtrl>();
    accCtr->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    std::string envId = "env123";

    // When
    TEHelper::saveTeamEnvID(accCtr, envId);

    // Then
    EXPECT_EQ(accCtr->taskAssignment.header.teamEnvironmentID, envId);
    EXPECT_EQ(accCtr->header.teamEnvironmentID, envId);

}

TEST_F(TestSaveTeamEnvID, UserInput) {
    // Given
    auto accCtr = std::make_shared<SYSAGENT::AccessCtrl>();
    accCtr->eventType = SYSAGENT::EventType::USERINPUT;
    std::string envId = "env123";

    // When
    TEHelper::saveTeamEnvID(accCtr, envId);

    // Then
    EXPECT_EQ(accCtr->userInput.header.teamEnvironmentID, envId);
    EXPECT_EQ(accCtr->header.teamEnvironmentID, envId);

}

TEST_F(TestSaveTeamEnvID, TaskStop) {
    // Given
    auto accCtr = std::make_shared<SYSAGENT::AccessCtrl>();
    accCtr->eventType = SYSAGENT::EventType::TASKSTOP;
    std::string envId = "env123";

    // When
    TEHelper::saveTeamEnvID(accCtr, envId);

    // Then
    EXPECT_EQ(accCtr->taskStop.header.teamEnvironmentID, envId);
    EXPECT_EQ(accCtr->header.teamEnvironmentID, envId);

}

TEST_F(TestSaveTeamEnvID, UnknownEventType) {
    // Given
    auto accCtr = std::make_shared<SYSAGENT::AccessCtrl>();
    accCtr->eventType = static_cast<SYSAGENT::EventType>(999); // Assuming 999 is not a valid EventType
    std::string envId = "env123";

    // When
    TEHelper::saveTeamEnvID(accCtr, envId);

    // Then
    // No changes expected as the eventType is not handled
    EXPECT_NE(accCtr->taskAssignment.header.teamEnvironmentID, envId);
    EXPECT_NE(accCtr->userInput.header.teamEnvironmentID, envId);
    EXPECT_NE(accCtr->taskStop.header.teamEnvironmentID, envId);
    EXPECT_NE(accCtr->header.teamEnvironmentID, envId);

}

