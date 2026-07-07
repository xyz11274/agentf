/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/WorkFlow.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/common_macro.h"
#include "queue/QueueManager.h"
#include "taskManage/AgentManager.h"
#include "taskManage/Task.h"
#include "taskManage/TaskEntity.h"
#include "taskManage/TaskNode.h"
#include <cstddef>
#include <string>

class TestSendUserOutputMsg: public ::testing::Test
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

TEST_F(TestSendUserOutputMsg, EmptyTeamEnvId) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "";

    // When
    WorkFlow workFlow;
    workFlow.sendUserOutputMsg(accessCtrl);


    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSendUserOutputMsg, NullTeamEnvironment) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "nonexistentID";

    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&))
    .stubs().will(returnValue(std::shared_ptr<TeamEnvironment>(nullptr)));

    // When
    WorkFlow workFlow;
    workFlow.sendUserOutputMsg(accessCtrl);


    // Clean
    GlobalMockObject::verify();
}

// TEST_F(TestSendUserOutputMsg, NewUserOutputFails) {
//     // Given
//     auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//     accessCtrl->header.teamEnvironmentID = "validID";

//     std::string teID = "teID";
//     const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
//     auto te = std::make_shared<TeamEnvironment>(teID, userInfo);

//     MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(te));
//     MOCKER(static_cast<std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::shared_ptr<TeamEnvironment>, std::string, std::string, int)>(&TEHelper::newUserOutput))
//     .stubs().will(returnValue(std::shared_ptr<SYSAGENT::AccessCtrl>(nullptr)));

//     // When
//     WorkFlow workFlow;
//     workFlow.sendUserOutputMsg(accessCtrl);


//     // Clean
//     GlobalMockObject::verify();
// }

TEST_F(TestSendUserOutputMsg, Success) {
    // Given
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "validID";

    std::string teID = "teID";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);
    te->session = new PerSession();

    auto userOutputMsg = std::make_shared<SYSAGENT::AccessCtrl>();

    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(te));
    MOCKER(static_cast<std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::shared_ptr<TeamEnvironment>, std::string, std::string, int)>(&TEHelper::newUserOutput)).stubs().will(returnValue(userOutputMsg));

    WorkFlow workFlow;
    workFlow.sender = "sender";
    workFlow.workFlowID = "workFlowID";
    workFlow.clientExField = "clientExField";

    // When
    workFlow.sendUserOutputMsg(accessCtrl);

    // Then
    EXPECT_EQ(userOutputMsg->extractSender(), "SystemAgent");
    EXPECT_EQ(userOutputMsg->extractReceiver(), "sender");
    EXPECT_EQ(userOutputMsg->extractTaskID(), "workFlowID");
//    EXPECT_EQ(userOutputMsg->getClientExField(), "clientExField");

    // Clean
    GlobalMockObject::verify();
    delete te->session;
}

