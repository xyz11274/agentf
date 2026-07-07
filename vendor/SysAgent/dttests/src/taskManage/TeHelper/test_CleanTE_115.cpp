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

class TestCleanTE: public ::testing::Test
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

TEST_F(TestCleanTE, AccessCtrlNull) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;
    PerSession session;

    // When
    TEHelper::CleanTE(accessCtrl, &session);


}

//TEST_F(TestCleanTE, TeamEnvIdEmpty) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    PerSession session;
//
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEManager::extractTeamEnvId)).stubs().will(returnValue(""));
//    MOCKER(static_cast<UserInfo(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractUserInfo)).stubs().will(returnValue(UserInfo{"testUser", "device1", "category1", "uuid1", "client1", "acm1"}));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::findTEByUserId)).stubs().will(returnValue(nullptr));
//
//    // When
//    TEHelper::CleanTE(accessCtrl, &session);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCleanTE, TeamEnvIdFound) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->taskAssignment.header.teamEnvironmentID = "teamEnv1";
//    PerSession session;
//
//    auto teamEnv = std::make_shared<TeamEnvironment>();
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEManager::extractTeamEnvId)).stubs().will(returnValue("teamEnv1"));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::getTEById)).stubs().will(returnValue(teamEnv));
//    MOCKER(static_cast<std::shared_ptr<WorkFlow>(*)(std::shared_ptr<TeamEnvironment>)>(&TeamEnvironment::getWorkFlow)).stubs().will(returnValue(nullptr));
//    MOCKER(static_cast<bool(*)(const std::string&)>(&TEManager::removeTE)).stubs().will(returnValue(true));
//    MOCKER(static_cast<void(*)(const std::string&)>(&GraphManager::DeleteAgentGraph)).stubs();
//
//    // When
//    TEHelper::CleanTE(accessCtrl, &session);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCleanTE, TeamEnvIdNotFound_UserFound) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::LOGINOUT;
//    PerSession session;
//
//    auto teamEnv = std::make_shared<TeamEnvironment>();
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEManager::extractTeamEnvId)).stubs().will(returnValue(""));
//    MOCKER(static_cast<UserInfo(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractUserInfo)).stubs().will(returnValue(UserInfo{"testUser", "device1", "category1", "uuid1", "client1", "acm1"}));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::findTEByUserId)).stubs().will(returnValue(teamEnv));
//    MOCKER(static_cast<std::shared_ptr<WorkFlow>(*)(std::shared_ptr<TeamEnvironment>)>(&TeamEnvironment::getWorkFlow)).stubs().will(returnValue(nullptr));
//    MOCKER(static_cast<bool(*)(const std::string&)>(&TEManager::removeTE)).stubs().will(returnValue(true));
//    MOCKER(static_cast<void(*)(const std::string&)>(&GraphManager::DeleteAgentGraph)).stubs();
//
//    // When
//    TEHelper::CleanTE(accessCtrl, &session);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCleanTE, TeamEnvIdNotFound_UserNotFound) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::LOGINOUT;
//    PerSession session;
//
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEManager::extractTeamEnvId)).stubs().will(returnValue(""));
//    MOCKER(static_cast<UserInfo(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEHelper::extractUserInfo)).stubs().will(returnValue(UserInfo{"testUser", "device1", "category1", "uuid1", "client1", "acm1"}));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::findTEByUserId)).stubs().will(returnValue(nullptr));
//
//    // When
//    TEHelper::CleanTE(accessCtrl, &session);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestCleanTE, TeamEnvIdFound_WorkFlowNotNull) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrl->taskAssignment.header.teamEnvironmentID = "teamEnv1";
//    PerSession session;
//
//    auto teamEnv = std::make_shared<TeamEnvironment>();
//    auto workFlow = std::make_shared<WorkFlow>();
//    auto stopMsg = std::make_shared<SYSAGENT::AccessCtrl>();
//    MOCKER(static_cast<std::string(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)>(&TEManager::extractTeamEnvId)).stubs().will(returnValue("teamEnv1"));
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>(&TEManager::getTEById)).stubs().will(returnValue(teamEnv));
//    MOCKER(static_cast<std::shared_ptr<WorkFlow>(*)(std::shared_ptr<TeamEnvironment>)>(&TeamEnvironment::getWorkFlow)).stubs().will(returnValue(workFlow));
//    MOCKER(static_cast<std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::shared_ptr<TeamEnvironment>)>(&TEHelper::newTaskStop)).stubs().will(returnValue(stopMsg));
//    MOCKER(static_cast<void(*)(std::shared_ptr<SYSAGENT::AccessCtrl>)>(&WorkFlow::stopTask)).stubs();
//    MOCKER(static_cast<bool(*)(const std::string&)>(&TEManager::removeTE)).stubs().will(returnValue(true));
//    MOCKER(static_cast<void(*)(const std::string&)>(&GraphManager::DeleteAgentGraph)).stubs();
//
//    // When
//    TEHelper::CleanTE(accessCtrl, &session);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}

