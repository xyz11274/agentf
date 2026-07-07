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

class TestRunGraph: public ::testing::Test
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

TEST_F(TestRunGraph, InvalidAccessCtrlPointer) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;

    // When
    TEHelper::RunGraph(accessCtrl);


}

//TEST_F(TestRunGraph, EmptyTEid) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrlData;
//    accessCtrlData.eventType = SYSAGENT::EventType::UNKNOWN;
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrlData);
//
//    // When
//    TEHelper::RunGraph(accessCtrl);
//
//
//}
//
//TEST_F(TestRunGraph, ValidTEidButNoAgentGraph) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrlData;
//    accessCtrlData.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrlData.taskAssignment.header.teamEnvironmentID = "teid1";
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrlData);
//
//    MOCKER(static_cast<GraphManager::MMAgentGraphPtr(*)(const std::string&, const std::string&)>( &GraphManager::GetAgentGraph)).stubs().will(returnValue(nullptr));
//
//    // When
//    TEHelper::RunGraph(accessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRunGraph, ValidTEidAndAgentGraph) {
//    // Given
//    SYSAGENT::AccessCtrl accessCtrlData;
//    accessCtrlData.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
//    accessCtrlData.taskAssignment.header.teamEnvironmentID = "teid1";
//    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>(accessCtrlData);
//
//    auto mockAgentGraph = std::make_shared<mmagentflow::MMAgentGraph>();
//    MOCKER(static_cast<GraphManager::MMAgentGraphPtr(*)(const std::string&, const std::string&)>( &GraphManager::GetAgentGraph)).stubs().will(returnValue(mockAgentGraph));
//
//    auto mockMessage = std::make_shared<mmagentflow::MMMessage>();
//    MOCKER(static_cast<std::shared_ptr<mmagentflow::MMMessage>(*)(std::shared_ptr<mmagentflow::MMAgentGraph>)>( &GraphUtils::NewMessage)).stubs().will(returnValue(mockMessage));
//
//    // When
//    TEHelper::RunGraph(accessCtrl);
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
