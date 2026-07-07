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

class TestSendNewTaskStatusReport: public ::testing::Test
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

//TEST_F(TestSendNewTaskStatusReport, sendNewTaskStatusReport_TeamEnvironmentNotFound) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "nonexistentID";
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>( &TEManager::getTEById)).stubs().with(smirror("nonexistentID")).will(returnValue(nullptr));
//
//    // When
//    TEHelper::sendNewTaskStatusReport(accessCtrl);
//
//    // Then
//    // No assertions needed as the function should return early
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestSendNewTaskStatusReport, sendNewTaskStatusReport_TaskMsgNull) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "validID";
//    auto te = std::make_shared<TeamEnvironment>();
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>( &TEManager::getTEById)).stubs().with(smirror("validID")).will(returnValue(te));
//    MOCKER(static_cast<std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::string, int, std::string)>( &TEHelper::newTaskStatusReport)).stubs().will(returnValue(nullptr));
//
//    // When
//    TEHelper::sendNewTaskStatusReport(accessCtrl);
//
//    // Then
//    // No assertions needed as the function should return early
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestSendNewTaskStatusReport, sendNewTaskStatusReport_WorkFlowNull) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "validID";
//    accessCtrl->header.taskID = "taskID";
//    auto te = std::make_shared<TeamEnvironment>();
//    auto taskMsg = std::make_shared<SYSAGENT::AccessCtrl>();
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>( &TEManager::getTEById)).stubs().with(smirror("validID")).will(returnValue(te));
//    MOCKER(static_cast<std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::string, int, std::string)>( &TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskMsg));
//    MOCKER(static_cast<std::shared_ptr<WorkFlow>(*)(const std::string&)>( &TeamEnvironment::getWorkFlow)).stubs().with(smirror("taskID")).will(returnValue(nullptr));
//
//    // When
//    TEHelper::sendNewTaskStatusReport(accessCtrl);
//
//    // Then
//    // No assertions needed as the function should return early
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestSendNewTaskStatusReport, sendNewTaskStatusReport_WorkFlowNotFinished) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "validID";
//    accessCtrl->header.taskID = "taskID";
//    auto te = std::make_shared<TeamEnvironment>();
//    auto taskMsg = std::make_shared<SYSAGENT::AccessCtrl>();
//    auto workFlow = std::make_shared<WorkFlow>();
//    workFlow->workFlowStatus = WorkFlowStatus::RUNNING;
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>( &TEManager::getTEById)).stubs().with(smirror("validID")).will(returnValue(te));
//    MOCKER(static_cast<std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::string, int, std::string)>( &TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskMsg));
//    MOCKER(static_cast<std::shared_ptr<WorkFlow>(*)(const std::string&)>( &TeamEnvironment::getWorkFlow)).stubs().with(smirror("taskID")).will(returnValue(workFlow));
//
//    // When
//    TEHelper::sendNewTaskStatusReport(accessCtrl);
//
//    // Then
//    // No assertions needed as the function should not delete the workFlow
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestSendNewTaskStatusReport, sendNewTaskStatusReport_WorkFlowFinished) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.teamEnvironmentID = "validID";
//    accessCtrl->header.taskID = "taskID";
//    auto te = std::make_shared<TeamEnvironment>();
//    auto taskMsg = std::make_shared<SYSAGENT::AccessCtrl>();
//    auto workFlow = std::make_shared<WorkFlow>();
//    workFlow->workFlowStatus = WorkFlowStatus::FINISHED;
//    MOCKER(static_cast<std::shared_ptr<TeamEnvironment>(*)(const std::string&)>( &TEManager::getTEById)).stubs().with(smirror("validID")).will(returnValue(te));
//    MOCKER(static_cast<std::shared_ptr<SYSAGENT::AccessCtrl>(*)(std::string, int, std::string)>( &TEHelper::newTaskStatusReport)).stubs().will(returnValue(taskMsg));
//    MOCKER(static_cast<std::shared_ptr<WorkFlow>(*)(const std::string&)>( &TeamEnvironment::getWorkFlow)).stubs().with(smirror("taskID")).will(returnValue(workFlow));
//
//    // When
//    TEHelper::sendNewTaskStatusReport(accessCtrl);
//
//    // Then
//    // No assertions needed as the function should delete the workFlow
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
