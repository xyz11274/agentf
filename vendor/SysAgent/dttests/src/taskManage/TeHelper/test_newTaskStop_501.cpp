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

class TestNewTaskStop: public ::testing::Test
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

//TEST_F(TestNewTaskStop, validTeamEnvironment) {
//    // Given
//    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>();
//    UserInfo userInfo = {"username", "deviceid", "teamid", "devicecategory", "uuid", "clientsessionid", "acmsessionid"};
//    EXPECT_CALL(*te, getUserInfo()).WillOnce(testing::Return(userInfo));
//    EXPECT_CALL(*te, getTeamId()).WillOnce(testing::Return("teamid"));
//    MOCKER(static_cast<std::string(*)()>(AisfPlat::Utils::GetCurrentTimestampInCustomFormat)).stubs().will(returnValue("2023-10-05T14:48:00Z"));
//
//    // When
//    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newTaskStop(te);
//
//    // Then
//    EXPECT_EQ(result->eventType, SYSAGENT::EventType::TASKSTOP);
//    EXPECT_EQ(result->header.messageType, "UserTaskManagement");
//    EXPECT_EQ(result->header.userName, "username");
//    EXPECT_EQ(result->header.deviceId, "deviceid");
//    EXPECT_EQ(result->header.teamEnvironmentID, "teamid");
//    EXPECT_EQ(result->header.deviceCategory, "devicecategory");
//    EXPECT_EQ(result->header.uuid, "uuid");
//    EXPECT_EQ(result->header.clientSessionId, "clientsessionid");
//    EXPECT_EQ(result->header.acmSessionId, "acmsessionid");
//    EXPECT_EQ(result->header.eventTimeStamp, "2023-10-05T14:48:00Z");
//    EXPECT_EQ(result->taskStop.header.messageType, "UserTaskManagement");
//    EXPECT_EQ(result->taskStop.header.userName, "username");
//    EXPECT_EQ(result->taskStop.header.deviceId, "deviceid");
//    EXPECT_EQ(result->taskStop.header.teamEnvironmentID, "teamid");
//    EXPECT_EQ(result->taskStop.header.deviceCategory, "devicecategory");
//    EXPECT_EQ(result->taskStop.header.uuid, "uuid");
//    EXPECT_EQ(result->taskStop.header.clientSessionId, "clientsessionid");
//    EXPECT_EQ(result->taskStop.header.acmSessionId, "acmsessionid");
//    EXPECT_EQ(result->taskStop.header.eventTimeStamp, "2023-10-05T14:48:00Z");
//
//    // Clean
//    GlobalMockObject::verify();
//}

