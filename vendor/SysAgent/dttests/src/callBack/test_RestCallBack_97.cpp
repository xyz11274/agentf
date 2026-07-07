/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "callBack/callBack.h"
#include "queue/QueueManager.h"
#include "TeHelper.h"
#include "TEManager.h"
#include "aisf_platform/common_define.h"
#include <iostream>
#include "AgentManager.h"

//class TestRestCallBack: public ::testing::Test
//{
//protected:
//   static void SetUpTestCase()
//   {
//   }
//
//   static void TearDownTestCase()
//   {
//   }
//
//   virtual void SetUp()
//   {
//   }
//
//   virtual void TearDown()
//   {
//       GlobalMockObject::verify();
//   }
//};
//
//TEST_F(TestRestCallBack, InvalidReqBody) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"invalid\":\"json\"}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("parse error")));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, false);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRestCallBack, EmptyTeamEnvironmentID) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"header\":{\"teamEnvironmentID\":\"\"}}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, false);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRestCallBack, NonExistentTeamEnvironment) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"header\":{\"teamEnvironmentID\":\"nonexistent\"}}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(nullptr));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, false);
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestRestCallBack, NonExistentWorkflow) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"header\":{\"teamEnvironmentID\":\"existent\", \"taskID\":\"nonexistent\"}}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//
//    std::string taskID = "taskID";
//    UserInfo userInfo{};
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>(taskID, userInfo)));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, std::shared_ptr<WorkFlow> (TeamEnvironment::*)(const std::string&)).stubs().will(returnValue(nullptr));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, false);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRestCallBack, TaskAssignmentWithEmptyTaskID) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"header\":{\"teamEnvironmentID\":\"existent\", \"taskID\":\"task1\", \"eventType\":\"TASKASSIGNMENT\"}, \"taskAssignment\":{\"data\":{\"userInputText\":\"input\"}}}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//    std::string taskID = "taskID";
//    UserInfo userInfo{};
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>(taskID, userInfo)));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, std::shared_ptr<WorkFlow> (TeamEnvironment::*)(const std::string&)).stubs().will(returnValue(std::make_shared<WorkFlow>()));
//    MOCKER_CPP(&WorkFlow::createSubTask, std::string (WorkFlow::*)(const TaskEntity&, const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(std::string("")));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, false);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRestCallBack, TaskAssignmentWithValidTaskID) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"header\":{\"teamEnvironmentID\":\"existent\", \"taskID\":\"task1\", \"eventType\":\"TASKASSIGNMENT\"}, \"taskAssignment\":{\"data\":{\"userInputText\":\"input\"}}}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//    std::string taskID = "taskID";
//    UserInfo userInfo{};
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>(taskID, userInfo)));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, std::shared_ptr<WorkFlow> (TeamEnvironment::*)(const std::string&)).stubs().will(returnValue(std::make_shared<WorkFlow>()));
//    MOCKER_CPP(&WorkFlow::createSubTask, std::string (WorkFlow::*)(const TaskEntity&, const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(std::string("newTaskID")));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, true);
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestRestCallBack, TaskStatusManagementWithFinishedWorkflow) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"header\":{\"teamEnvironmentID\":\"existent\", \"taskID\":\"task1\", \"eventType\":\"TASKSTATUSMANAGEMENT\"}}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>()));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, std::shared_ptr<WorkFlow> (TeamEnvironment::*)(const std::string&)).stubs().will(returnValue(std::make_shared<WorkFlow>()));
//    MOCKER_CPP(&WorkFlow::changeStatus, void (WorkFlow::*)(const std::string&, const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(invoke([](const std::string&, const std::shared_ptr<SYSAGENT::AccessCtrl>&) { /* do nothing */ }));
//    MOCKER_CPP(&WorkFlow::getWorkFlowStatus, WorkFlowStatus (WorkFlow::*)()).stubs().will(returnValue(WorkFlowStatus::FINISHED));
//    MOCKER_CPP(&TeamEnvironment::deleteWorkFlow, void (TeamEnvironment::*)()).stubs().will(invoke([]() { /* do nothing */ }));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, true);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestRestCallBack, TaskStatusManagementWithUnfinishedWorkflow) {
//    // Given
//    std::string reqUrl = "http://example.com";
//    std::string reqBody = "{\"header\":{\"teamEnvironmentID\":\"existent\", \"taskID\":\"task1\", \"eventType\":\"TASKSTATUSMANAGEMENT\"}}";
//    std::map<std::string, std::string> reqValue;
//    RestRspInfo rspInfo;
//    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));
//    MOCKER_CPP(&TEManager::getTEById, std::shared_ptr<TeamEnvironment> (TEManager::*)(const std::string&)).stubs().will(returnValue(std::make_shared<TeamEnvironment>()));
//    MOCKER_CPP(&TeamEnvironment::getWorkFlow, std::shared_ptr<WorkFlow> (TeamEnvironment::*)(const std::string&)).stubs().will(returnValue(std::make_shared<WorkFlow>()));
//    MOCKER_CPP(&WorkFlow::changeStatus, void (WorkFlow::*)(const std::string&, const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(invoke([](const std::string&, const std::shared_ptr<SYSAGENT::AccessCtrl>&) { /* do nothing */ }));
//    MOCKER_CPP(&WorkFlow::getWorkFlowStatus, WorkFlowStatus (WorkFlow::*)()).stubs().will(returnValue(WorkFlowStatus::RUNNING));
//
//    // When
//    bool ret = SysCallBack::RestCallBack(reqUrl, reqBody, reqValue, rspInfo);
//
//    // Then
//    EXPECT_EQ(ret, true);
//
//    // Clean
//    GlobalMockObject::verify();
//}

