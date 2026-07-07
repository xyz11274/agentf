///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "ruleCognition/SpecifyIntentRuleHandler.h"
//#include "config/Config.h"
//#include "graphManager/GraphData.h"
//#include "taskManage/WorkFlow.h"
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
//TEST_F(TestDoChain, AccessCtrlIsNull) {
//    // Given
//    std::shared_ptr<AccessCtrl> accessCtrl = nullptr;
//
//    // When
//    SpecifyIntentRuleHandler specifyIntentRuleHandler;
//    std::shared_ptr<Rule> result = specifyIntentRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}
//
//TEST_F(TestDoChain, UserSpecifiedAgentEmpty) {
//    // Given
//    AccessCtrl accessCtrl;
//    accessCtrl.eventType = EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.data.userSpecifiedAgent = "";
//
//    // When
//    SpecifyIntentRuleHandler specifyIntentRuleHandler;
//    std::shared_ptr<Rule> result = specifyIntentRuleHandler.doChain(std::make_shared<AccessCtrl>(accessCtrl));
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}

//TEST_F(TestDoChain, UserSpecifiedAgentNotFound) {
//    // Given
//    AccessCtrl accessCtrl;
//    accessCtrl.eventType = EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.data.userSpecifiedAgent = "unknownAgent";
//
//    MOCKER(static_cast<std::string(*)(const std::string&)>(SpecifyIntentRuleHandler::userSpecifiedAgentRecognition))
//        .stubs()
//        .with(smirror("unknownAgent"))
//        .will(returnValue(""));
//
//    // When
//    SpecifyIntentRuleHandler specifyIntentRuleHandler;
//    std::shared_ptr<Rule> result = specifyIntentRuleHandler.doChain(std::make_shared<AccessCtrl>(accessCtrl));
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}

//TEST_F(TestDoChain, UserSpecifiedAgentFound) {
//    // Given
//    AccessCtrl accessCtrl;
//    accessCtrl.eventType = EventType::TASKASSIGNMENT;
//    accessCtrl.taskAssignment.data.userSpecifiedAgent = "knownAgent";
//    accessCtrl.taskAssignment.data.userInputText = "sample text";
//
//    MOCKER(static_cast<std::string(*)(const std::string&)>(SpecifyIntentRuleHandler::userSpecifiedAgentRecognition))
//        .stubs()
//        .with(smirror("knownAgent"))
//        .will(returnValue(std::string("agentType"));
//
//    // When
//    SpecifyIntentRuleHandler specifyIntentRuleHandler;
//    std::shared_ptr<Rule> result = specifyIntentRuleHandler.doChain(std::make_shared<AccessCtrl>(accessCtrl));
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->ruleName, RuleName::SPECIFY_INTENT_RULE_HANDLER);
//    EXPECT_EQ(result->nextNode, GRAPH_NODE_PlanNode);
//    EXPECT_EQ(result->intentTasks.size(), 1);
//    EXPECT_EQ(result->intentTasks[0].receiver, "agentType");
//    EXPECT_EQ(result->intentTasks[0].userInputText, "sample text");
//
//    // Clean
//    GlobalMockObject::verify();
//}

