///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "ruleCognition/RuleHandlerManager.h"
//#include "ruleCognition/RAGRuleHandler.h"
//#include "ruleCognition/TaskRuleHandler.h"
//#include "ruleCognition/CustomIntentRuleHandler.h"
//#include "ruleCognition/SpecifyIntentRuleHandler.h"
//
//class TestInit: public ::testing::Test
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
//TEST_F(TestInit, ut_name) {
//    // Given
//    RuleHandlerManager& ruleHandlerManager = RuleHandlerManager::getInstance();
//
//    // When
//    ruleHandlerManager.init();
//
//    // Then
//    EXPECT_EQ(ruleHandlerManager.ruleHandlers.size(), 4);
////    EXPECT_EQ(typeid(*ruleHandlerManager.ruleHandlers[0]).name(), typeid(RAGRuleHandler).name());
////    EXPECT_EQ(typeid(*ruleHandlerManager.ruleHandlers[1]).name(), typeid(TaskRuleHandler).name());
////    EXPECT_EQ(typeid(*ruleHandlerManager.ruleHandlers[2]).name(), typeid(SpecifyIntentRuleHandler).name());
////    EXPECT_EQ(typeid(*ruleHandlerManager.ruleHandlers[3]).name(), typeid(CustomIntentRuleHandler).name());
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
