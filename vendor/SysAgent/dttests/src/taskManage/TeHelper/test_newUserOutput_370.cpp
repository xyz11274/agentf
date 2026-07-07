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

class TestNewUserOutput: public ::testing::Test
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

TEST_F(TestNewUserOutput, NullTeamEnvironment) {
    // Given
    std::shared_ptr<TeamEnvironment> te = nullptr;
    std::string output = "test output";
    std::string status = "active";
    int flag = 1;

    // When
    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newUserOutput(te, output, status, flag);

    // Then
    EXPECT_EQ(result, nullptr);

}

//TEST_F(TestNewUserOutput, ValidTeamEnvironment) {
//    // Given
//    auto te = std::make_shared<TeamEnvironment>();
//    UserInfo user = {"user1", "device1", "team1", "category1", "uuid1", "client1", "acm1"};
//    te->setUserInfo(user);
//    std::string output = "test output";
//    std::string status = "active";
//    int flag = 1;
//
//    // When
//    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newUserOutput(te, output, status, flag);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->eventType, SYSAGENT::EventType::USEROUTPUT);
//    EXPECT_EQ(result->userOutput.header.userName, user.userName);
//    EXPECT_EQ(result->userOutput.header.deviceId, user.deviceId);
//    EXPECT_EQ(result->userOutput.header.teamEnvironmentID, te->getTeamId());
//    EXPECT_EQ(result->userOutput.header.deviceCategory, user.deviceCategory);
//    EXPECT_EQ(result->userOutput.header.uuid, user.uuid);
//    EXPECT_EQ(result->userOutput.header.clientSessionId, user.clientSessionId);
//    EXPECT_EQ(result->userOutput.header.acmSessionId, user.acmSessionId);
//    EXPECT_EQ(result->userOutput.data.userOutputText, output);
//    EXPECT_EQ(result->userOutput.data.streamStatus, status);
//    EXPECT_EQ(result->userOutput.data.userOutPutFlag, flag);
//
//}

//TEST_F(TestNewUserOutput, MockGetCurrentTimestampInCustomFormat) {
//    // Given
//    auto te = std::make_shared<TeamEnvironment>();
//    UserInfo user = {"user1", "device1", "team1", "category1", "uuid1", "client1", "acm1"};
//    te->setUserInfo(user);
//    std::string output = "test output";
//    std::string status = "active";
//    int flag = 1;
//    std::string mockTimestamp = "2023-10-05T14:48:00Z";
//
//    // Mock
//    MOCKER(static_cast<std::string(*)()>(AisfPlat::Utils::GetCurrentTimestampInCustomFormat)).stubs().will(returnValue(mockTimestamp));
//
//    // When
//    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newUserOutput(te, output, status, flag);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->userOutput.header.eventTimeStamp, mockTimestamp);
//
//    // Clean
//    GlobalMockObject::verify();
//}

