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

//TEST_F(TestNewUserOutput, InvalidTeid) {
//    // Given
//    std::string teid = "";
//    std::string output = "Sample Output";
//    std::string status = "Active";
//    int flag = 1;
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)(&TEManager::getTEById)).stubs().with(smirror(teid)).will(returnValue(nullptr));
//
//    // When
//    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newUserOutput(teid, output, status, flag);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestNewUserOutput, ValidTeid) {
//    // Given
//    std::string teid = "valid_teid";
//    std::string output = "Sample Output";
//    std::string status = "Active";
//    int flag = 1;
//    auto te = std::make_shared<TeamEnvironment>();
//    te->setUserInfo(UserInfo{"username", "device_id", "category", "uuid", "client_session_id", "acm_session_id"});
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)(&TEManager::getTEById)).stubs().with(smirror(teid)).will(returnValue(te));
//
//    // When
//    std::shared_ptr<SYSAGENT::AccessCtrl> result = TEHelper::newUserOutput(teid, output, status, flag);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->eventType, SYSAGENT::EventType::USEROUTPUT);
//    EXPECT_EQ(result->userOutput.data.userOutputText, output);
//    EXPECT_EQ(result->userOutput.data.streamStatus, status);
//    EXPECT_EQ(result->userOutput.data.userOutPutFlag, flag);
//
//    // Clean
//    GlobalMockObject::verify();
//}

