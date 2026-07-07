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

class TestGetOrCreateTE: public ::testing::Test
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

TEST_F(TestGetOrCreateTE, AccessCtrlNull) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;
    PerSession session;

    // When
    std::shared_ptr<TeamEnvironment> result = TEHelper::GetOrCreateTE(accessCtrl, &session);

    // Then
    EXPECT_EQ(result, nullptr);

    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestGetOrCreateTE, TeamEnvIdFound) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->taskAssignment.header.teamEnvironmentID = "te_123";
//    PerSession session;
//
//    MOCKER_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(std::string("te_123")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>()));
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = TEHelper::GetOrCreateTE(accessCtrl, &session);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestGetOrCreateTE, TeamEnvIdNotFound_TaskIdFound) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->taskAssignment.header.taskID = "task_123";
//    PerSession session;
//
//    Mocker_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(""));
//    Mocker_CPP(&TEHelper::extractTaskId, std::string (TEHelper::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue("task_123"));
//    Mocker_CPP(&TEManager::findTEByTaskId, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>()));
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = TEHelper::GetOrCreateTE(accessCtrl, &session);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestGetOrCreateTE, TeamEnvIdNotFound_TaskIdNotFound_CreateNew) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    PerSession session;
//
//    Mocker_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(""));
//    Mocker_CPP(&TEHelper::extractTaskId, std::string (TEHelper::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(""));
//    Mocker_CPP(&TEManager::createTEFromData, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>()));
//    Mocker(&GraphManager::CreateAgentGraph).stubs().will(returnValue(true));
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = TEHelper::GetOrCreateTE(accessCtrl, &session);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestGetOrCreateTE, NoSession) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->taskAssignment.header.teamEnvironmentID = "te_123";
//
//    Mocker_CPP(&TEManager::extractTeamEnvId, std::string (TEManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue("te_123"));
//    Mocker_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>()));
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = TEHelper::GetOrCreateTE(accessCtrl, nullptr);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}

