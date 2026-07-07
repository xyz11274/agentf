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

class TestAddOriginInputToKMM: public ::testing::Test
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

TEST_F(TestAddOriginInputToKMM, teIsNull) {
    // Given
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "";
    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs()
    .will(returnValue(std::shared_ptr<TeamEnvironment>(nullptr)));

    // When
    WorkFlow workFlow;
    workFlow.addOriginInputToKMM(accessCtrl);


    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddOriginInputToKMM, taskAssignment) {
    // Given
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "validID";
    accessCtrl->header.receiver = "SystemAgent";
    accessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    accessCtrl->taskAssignment.data.userInputText = "testInput";
    accessCtrl->header.eventTimeStamp = "2023-10-05T14:48:00Z";

    std::string teID = "teID";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);

    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(te));
    MOCKER_CPP(&QueueManager::postKmmMsgReadQueue, void (QueueManager::*)(const Memory&)).stubs();

    // When
    WorkFlow workFlow;
    workFlow.addOriginInputToKMM(accessCtrl);

    // Then
    Memory expectedMemory = {
        .userId = "user1",
        .sessionId = "session1",
        .role = "user",
        .content = "testInput",
        .date = "2023-10-05 14:48:00"
    };
    GlobalMockObject::verify();
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).count(), 1);
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).arg<0>(0), expectedMemory);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddOriginInputToKMM, userInputNotEmpty) {
    // Given
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "validID";
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
    accessCtrl->userInput.data.userInputText = "testInput";
    accessCtrl->header.eventTimeStamp = "2023-10-05T14:48:00Z";

    std::string teID = "teID";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);

    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(te));
    MOCKER_CPP(&QueueManager::postKmmMsgReadQueue, void (QueueManager::*)(const Memory&)).stubs();

    // When
    WorkFlow workFlow;
    workFlow.addOriginInputToKMM(accessCtrl);

    // Then
    Memory expectedMemory = {
        .userId = "user1",
        .sessionId = "session1",
        .role = "user",
        .content = "testInput",
        .date = "2023-10-05 14:48:00"
    };
    GlobalMockObject::verify();
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).count(), 1);
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).arg<0>(0), expectedMemory);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddOriginInputToKMM, userInputEmpty) {
    // Given
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "validID";
    accessCtrl->eventType = SYSAGENT::EventType::USERINPUT;
    accessCtrl->userInput.data.userInputText = "";
    accessCtrl->header.eventTimeStamp = "2023-10-05T14:48:00Z";

    std::string teID = "teID";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);


    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(te));
    MOCKER_CPP(&QueueManager::postKmmMsgReadQueue, void (QueueManager::*)(const Memory&)).stubs();

    // When
    WorkFlow workFlow;
    workFlow.addOriginInputToKMM(accessCtrl);

    // Then
    GlobalMockObject::verify();
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).count(), 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddOriginInputToKMM, userOutputClarification) {
    // Given
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "validID";
    accessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
    accessCtrl->userOutput.data.userOutPutFlag = 2;
    accessCtrl->userOutput.data.userOutputText = "testOutput";
    accessCtrl->header.eventTimeStamp = "2023-10-05T14:48:00Z";

    std::string teID = "teID";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);


    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(te));
    MOCKER_CPP(&QueueManager::postKmmMsgReadQueue, void (QueueManager::*)(const Memory&)).stubs();

    // When
    WorkFlow workFlow;
    workFlow.addOriginInputToKMM(accessCtrl);

    // Then
    Memory expectedMemory = {
        .userId = "user1",
        .sessionId = "session1",
        .role = "system",
        .content = "testOutput",
        .date = "2023-10-05 14:48:00"
    };
    GlobalMockObject::verify();
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).count(), 1);
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).arg<0>(0), expectedMemory);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestAddOriginInputToKMM, userOutputNotClarification) {
    // Given
    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "validID";
    accessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
    accessCtrl->userOutput.data.userOutPutFlag = 1;
    accessCtrl->userOutput.data.userOutputText = "testOutput";
    accessCtrl->header.eventTimeStamp = "2023-10-05T14:48:00Z";

    std::string teID = "teID";
    const UserInfo userInfo = {.userName = "user1", .clientSessionId = "session1"};
    auto te = std::make_shared<TeamEnvironment>(teID, userInfo);


    MOCKER_CPP(&TEManager::getTEById, shared_ptr<TeamEnvironment> (TEManager::*)(const string&)).stubs().will(returnValue(te));
    MOCKER_CPP(&QueueManager::postKmmMsgReadQueue, void (QueueManager::*)(const Memory&)).stubs();

    // When
    WorkFlow workFlow;
    workFlow.addOriginInputToKMM(accessCtrl);

    // Then
    GlobalMockObject::verify();
//    EXPECT_EQ(GlobalMockObject::callsTo(&QueueManager::postKmmMsgReadQueue).count(), 0);

    // Clean
    GlobalMockObject::verify();
}

