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

//TEST_F(TestSendUserOutputMsg, TeamEnvironmentNotFound) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "nonexistentID";
//    string content = "test content";
//    string status = "test status";
//    int flag = 1;
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(nullptr));
//
//    // When
//    TEHelper::sendUserOutputMsg(accessCtrl, content, status, flag);
//
//    // Then
//    // No outputMsg should be created or posted
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestSendUserOutputMsg, NewUserOutputReturnsNull) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "validID";
//    string content = "test content";
//    string status = "test status";
//    int flag = 1;
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>();
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(te));
//    MOCKER_CPP(&TEHelper::newUserOutput, std::shared_ptr<SYSAGENT::AccessCtrl> (TEHelper::*)(std::shared_ptr<TeamEnvironment>, std::string, std::string, int)).stubs().will(returnValue(nullptr));
//
//    // When
//    TEHelper::sendUserOutputMsg(accessCtrl, content, status, flag);
//
//    // Then
//    // No outputMsg should be posted
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestSendUserOutputMsg, ValidInput) {
//    // Given
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "validID";
//    accessCtrl->header.taskID = "task123";
//    accessCtrl->extractSender = []() { return "sender123"; };
//    string content = "test content";
//    string status = "test status";
//    int flag = 1;
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>();
//    te->session = new PerSession();
//    std::shared_ptr<SYSAGENT::AccessCtrl> outputMsg = std::make_shared<SYSAGENT::AccessCtrl>();
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(te));
//    MOCKER_CPP(&TEHelper::newUserOutput, std::shared_ptr<SYSAGENT::AccessCtrl> (TEHelper::*)(std::shared_ptr<TeamEnvironment>, std::string, std::string, int)).stubs().will(returnValue(outputMsg));
//    MOCKER_CPP(&QueueManager::postWriteQueue, void (QueueManager::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&, PerSession*)).stubs().will(returnValue());
//
//    // When
//    TEHelper::sendUserOutputMsg(accessCtrl, content, status, flag);
//
//    // Then
//    EXPECT_EQ(outputMsg->header.taskID, "task123");
//    EXPECT_EQ(outputMsg->userOutput.header.taskID, "task123");
//    EXPECT_EQ(outputMsg->header.userName, te->getUserInfo().userName);
//    EXPECT_EQ(outputMsg->header.deviceId, te->getUserInfo().deviceId);
//    EXPECT_EQ(outputMsg->header.teamEnvironmentID, te->getTeamId());
//    EXPECT_EQ(outputMsg->header.deviceCategory, te->getUserInfo().deviceCategory);
//    EXPECT_EQ(outputMsg->header.uuid, te->getUserInfo().uuid);
//    EXPECT_EQ(outputMsg->header.clientSessionId, te->getUserInfo().clientSessionId);
//    EXPECT_EQ(outputMsg->header.acmSessionId, te->getUserInfo().acmSessionId);
//    EXPECT_EQ(outputMsg->userOutput.data.userOutputText, content);
//    EXPECT_EQ(outputMsg->userOutput.data.streamStatus, status);
//    EXPECT_EQ(outputMsg->userOutput.data.userOutPutFlag, flag);
//    EXPECT_EQ(outputMsg->sender, "SystemAgent");
//    EXPECT_EQ(outputMsg->receiver, "sender123");
//
//    // Clean
//    GlobalMockObject::verify();
//    delete te->session;
//}

