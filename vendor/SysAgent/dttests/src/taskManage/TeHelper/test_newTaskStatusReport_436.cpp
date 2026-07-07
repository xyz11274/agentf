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

TEST_F(TestNewTaskStatusReport, EmptyTeid) {
    // Given
    std::string teid = "";
    int code = 200;
    std::string report = "Success";

    // When
    auto result = TEHelper::newTaskStatusReport(teid, code, report);

    // Then
    EXPECT_EQ(result, nullptr);

}

//TEST_F(TestNewTaskStatusReport, NonExistentTeid) {
//    // Given
//    std::string teid = "nonexistent";
//    int code = 200;
//    std::string report = "Success";
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().with(smirror(teid)).will(returnValue(nullptr));
//
//    // When
//    auto result = TEHelper::newTaskStatusReport(teid, code, report);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestNewTaskStatusReport, ValidTeid) {
//    // Given
//    std::string teid = "valid";
//    int code = 200;
//    std::string report = "Success";
//    auto te = std::make_shared<TeamEnvironment>();
//    UserInfo user = {"username", "deviceid", "category", "uuid", "clientsessionid", "acmsessionid"};
//    te->setUserInfo(user);
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().with(smirror(teid)).will(returnValue(te));
//    std::string timestamp = "2023-10-05T14:48:00Z";
//    MOCKER(static_cast<std::string(*)()>(AisfPlat::Utils::GetCurrentTimestampInCustomFormat)).stubs().will(returnValue(timestamp));
//
//    // When
//    auto result = TEHelper::newTaskStatusReport(teid, code, report);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->header.messageType, "TaskStatusManagement");
//    EXPECT_EQ(result->header.userName, user.userName);
//    EXPECT_EQ(result->header.deviceId, user.deviceId);
//    EXPECT_EQ(result->header.teamEnvironmentID, teid);
//    EXPECT_EQ(result->header.deviceCategory, user.deviceCategory);
//    EXPECT_EQ(result->header.uuid, user.uuid);
//    EXPECT_EQ(result->header.clientSessionId, user.clientSessionId);
//    EXPECT_EQ(result->header.acmSessionId, user.acmSessionId);
//    EXPECT_EQ(result->header.eventTimeStamp, timestamp);
//    EXPECT_EQ(result->taskStatusManagement.header.messageType, "TaskStatusManagement");
//    EXPECT_EQ(result->taskStatusManagement.header.userName, user.userName);
//    EXPECT_EQ(result->taskStatusManagement.header.deviceId, user.deviceId);
//    EXPECT_EQ(result->taskStatusManagement.header.teamEnvironmentID, teid);
//    EXPECT_EQ(result->taskStatusManagement.header.deviceCategory, user.deviceCategory);
//    EXPECT_EQ(result->taskStatusManagement.header.uuid, user.uuid);
//    EXPECT_EQ(result->taskStatusManagement.header.clientSessionId, user.clientSessionId);
//    EXPECT_EQ(result->taskStatusManagement.header.acmSessionId, user.acmSessionId);
//    EXPECT_EQ(result->taskStatusManagement.header.eventTimeStamp, timestamp);
//    EXPECT_EQ(result->taskStatusManagement.data.messageType, "TaskStatusReport");
//    EXPECT_EQ(result->taskStatusManagement.data.taskStatusCode, code);
//    EXPECT_EQ(result->taskStatusManagement.data.taskStatusReport, report);
//
//    // Clean
//    GlobalMockObject::verify();
//}

