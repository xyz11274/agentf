///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "ruleCognition/CustomIntentRuleHandler.h"
//#include "config/Config.h"
//#include "ruleCognition/RuleConfigManager.h"
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
//TEST_F(TestDoChain, testDoChainWithNullAccessCtrl) {
//
//    // When
//    CustomIntentRuleHandler customIntentRuleHandler;
//    shared_ptr<Rule> result = customIntentRuleHandler.doChain(nullptr);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}
//
//TEST_F(TestDoChain, testDoChainWithNoMatchingRule) {
//    // Given
//    AccessCtrl accessCtrl;
//    accessCtrl.header = MsgAisfHeader{.taskID="taskID"};
//    MOCKER_CPP(&RuleConfigManager::match, shared_ptr<Rule> (RuleConfigManager::*)(shared_ptr<AisfPlat::MsgAisfHeader>& header)).stubs().will(
//            returnValue(nullptr));
//
//    // When
//    CustomIntentRuleHandler customIntentRuleHandler;
//    shared_ptr<Rule> result = customIntentRuleHandler.doChain(make_shared<AccessCtrl>(accessCtrl));
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestDoChain, testDoChainWithMatchingRule) {
//    // Given
//    AccessCtrl accessCtrl;
//    accessCtrl.header = MsgAisfHeader{.taskID="taskID"};
//    shared_ptr<Rule> expectedRule = make_shared<Rule>();
//    expectedRule->ruleName = RuleName::CUSTOM_INTENT_RULE_HANDLER;
//    expectedRule->nextNode = "nextNode";
//    MOCKER_CPP(&RuleConfigManager::match, shared_ptr<Rule> (RuleConfigManager::*)(shared_ptr<AisfPlat::MsgAisfHeader>& header)).stubs().will(
//            returnValue(expectedRule));
//
//    // When
//    CustomIntentRuleHandler customIntentRuleHandler;
//    shared_ptr<Rule> result = customIntentRuleHandler.doChain(make_shared<AccessCtrl>(accessCtrl));
//
//    // Then
//    EXPECT_EQ(result->ruleName, expectedRule->ruleName);
//    EXPECT_EQ(result->nextNode, expectedRule->nextNode);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
