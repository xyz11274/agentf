///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "taskManage/TaskNode.h"
//
//class TestExecTask: public ::testing::Test
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
//TEST_F(TestExecTask, validTaskAndAccessCtrl) {
//    // Given
//    auto task = make_shared<Task>();
//    TaskNode taskNode(task);
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//
//    MOCKER_CPP(&Task::exec, void (Task::*)(shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(returnValue());
//
//    // When
//    taskNode.execTask(accessCtrl);
//
//    // Then
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExecTask, nullTask) {
//    // Given
//    TaskNode taskNode(nullptr);
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//
//    MOCKER_CPP(&Task::exec, void (Task::*)(shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(returnValue());
//
//    // When
//    taskNode.execTask(accessCtrl);
//
//    // Then
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestExecTask, nullAccessCtrl) {
//    // Given
//    auto task = make_shared<Task>();
//    TaskNode taskNode(task);
//    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;
//
//    MOCKER_CPP(&Task::exec, void (Task::*)(shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(returnValue());
//
//    // When
//    taskNode.execTask(accessCtrl);
//
//    // Then
//    GlobalMockObject::verify();
//}
//
