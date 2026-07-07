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

class TestNewTaskStatusReport: public ::testing::Test
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

TEST_F(TestNewTaskStatusReport, TeamEnvironmentIsNull) {
    // Given
    std::shared_ptr<TeamEnvironment> te = nullptr;
    int code = 200;
    std::string report = "Success";

    // When
    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newTaskStatusReport(te, code, report);

    // Then
    EXPECT_EQ(result, nullptr);

}

//TEST_F(TestNewTaskStatusReport, TeamEnvironmentIsValid) {
//    // Given
//    auto te = std::make_shared<TeamEnvironment>();
//    UserInfo user = {"user1", "device1", "team1", "category1", "uuid1", "client1", "acm1"};
//    te->setUserInfo(user);
//    int code = 200;
//    std::string report = "Success";
//
//    // When
//    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newTaskStatusReport(te, code, report);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->eventType, SYSAGENT::EventType::TASKSTATUSMANAGEMENT);
//    EXPECT_EQ(result->header.userName, user.userName);
//    EXPECT_EQ(result->header.deviceId, user.deviceId);
//    EXPECT_EQ(result->header.teamEnvironmentID, te->getTeamId());
//    EXPECT_EQ(result->header.deviceCategory, user.deviceCategory);
//    EXPECT_EQ(result->header.uuid, user.uuid);
//    EXPECT_EQ(result->header.clientSessionId, user.clientSessionId);
//    EXPECT_EQ(result->header.acmSessionId, user.acmSessionId);
//    EXPECT_EQ(result->taskStatusManagement.data.messageType, "TaskStatusReport");
//    EXPECT_EQ(result->taskStatusManagement.data.taskStatusCode, code);
//    EXPECT_EQ(result->taskStatusManagement.data.taskStatusReport, report);
//
//}

