///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "ruleCognition/TaskRuleHandler.h"
//
//class TestDoChain: public ::testing::Test
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
//TEST_F(TestDoChain, doChain_NullptrAccessCtrl) {
//    // Given
//    shared_ptr<AccessCtrl> accessCtrl = nullptr;
//
//    // When
//    TaskRuleHandler taskRuleHandler;
//    shared_ptr<Rule> result = taskRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}
//
//TEST_F(TestDoChain, doChain_EmptyTaskID) {
//    // Given
//    AccessCtrl accessCtrl;
//    accessCtrl.header.taskID = "";
//
//    // When
//    TaskRuleHandler taskRuleHandler;
//    shared_ptr<Rule> result = taskRuleHandler.doChain(make_shared<AccessCtrl>(accessCtrl));
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}
//
//TEST_F(TestDoChain, doChain_ValidTaskID) {
//    // Given
//    AccessCtrl accessCtrl;
//    accessCtrl.header.taskID = "validTaskID";
//
//    // When
//    TaskRuleHandler taskRuleHandler;
//    shared_ptr<Rule> result = taskRuleHandler.doChain(make_shared<AccessCtrl>(accessCtrl));
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->ruleName, RuleName::TASK_RULE_HANDLER);
//    EXPECT_EQ(result->nextNode, "ExecuteNode");
//
//}
//
