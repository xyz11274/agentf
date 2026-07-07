///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "ruleCognition/RAGRuleHandler.h"
//#include "TEManager.h"
//#include "TeHelper.h"
//#include "graphManager/GraphData.h"
//#include "memory/SafetyFence.h"
//#include "queue/QueueManager.h"
//#include "ruleCognition/Rule.h"
//#include "ruleCognition/CommonDefinition.h"
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
//TEST_F(TestDoChain, testDoChainNullAccessCtrl) {
//    // Given
//    RAGRuleHandler ragRuleHandler;
//
//    // When
//    auto result = ragRuleHandler.doChain(nullptr);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}
//
//TEST_F(TestDoChain, testDoChainSafetyFenceFlagClose) {
//    // Given
//    RAGRuleHandler ragRuleHandler;
//    auto accessCtrl = std::make_shared<AccessCtrl>();
//    MOCKER(static_cast<bool(*)(const string&, string&, const string&)>(&AisfPlat::Utils::GetEnvVariable))
//        .stubs().with(smirror(ENV_RAG_SAFETYFENCE_FLAG), outBound(string()), smirror(CLOSE)).will(returnValue(false));
//
//    // When
//    auto result = ragRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestDoChain, testDoChainEmptyRequest) {
//    // Given
//    RAGRuleHandler ragRuleHandler;
//    auto accessCtrl = std::make_shared<AccessCtrl>();
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "team1";
//    MOCKER(static_cast<bool(*)(const string&, string&, const string&)>(&AisfPlat::Utils::GetEnvVariable))
//        .stubs().with(smirror(ENV_RAG_SAFETYFENCE_FLAG), outBound(string()), smirror(OPEN)).will(returnValue(true));
//
//    // When
//    auto result = ragRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestDoChain, testDoChainTaskAssignmentNoResult) {
//    // Given
//    RAGRuleHandler ragRuleHandler;
//    auto accessCtrl = std::make_shared<AccessCtrl>();
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "team1";
////    accessCtrl->extractInput = []() { return "request"; };
//    accessCtrl->eventType = EventType::TASKASSIGNMENT;
//    MOCKER(static_cast<bool(*)(const string&, string&, const string&)>(&AisfPlat::Utils::GetEnvVariable))
//        .stubs().with(smirror(ENV_RAG_SAFETYFENCE_FLAG), outBound(string()), smirror(OPEN)).will(returnValue(true));
//    MOCKER_CPP(&SafetyFence::SimilaritySearch, std::vector<std::string>(*)(std::string, std::vector<std::string>))
//        .stubs().will(returnValue(std::vector<std::string>()));
//
//    // When
//    auto result = ragRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestDoChain, testDoChainTaskAssignmentRejectedAnswer) {
//    // Given
//    RAGRuleHandler ragRuleHandler;
//    auto accessCtrl = std::make_shared<AccessCtrl>();
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "team1";
////    accessCtrl->extractInput = []() { return "request"; };
//    accessCtrl->eventType = EventType::TASKASSIGNMENT;
//    MOCKER(static_cast<bool(*)(const string&, string&, const string&)>(&AisfPlat::Utils::GetEnvVariable))
//        .stubs().with(smirror(ENV_RAG_SAFETYFENCE_FLAG), outBound(string()), smirror(OPEN)).will(returnValue(true));
//    MOCKER_CPP(&SafetyFence::SimilaritySearch, std::vector<std::string>(*)(std::string, std::vector<std::string>))
//        .stubs().will(returnValue(std::vector<std::string>{"0.1", "query", "{\"intentName\": \"REJECTED_ANSWER\", \"data\": {\"content\": \"rejected content\"}}" }));
//    MOCKER_CPP(&SafetyFence::parseIntentName, std::string(*)(const std::string&))
//        .stubs().will(returnValue(std::string("REJECTED_ANSWER")));
//    MOCKER_CPP(&SafetyFence::parseRejectContent, std::string(*)(const std::string&))
//        .stubs().will(returnValue(std::string("rejected content")));
////    MOCKER_CPP(&TEHelper::sendUserOutputMsg, void(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&, std::string, std::string, int))
////        .stubs().will(returnValue());
////    MOCKER_CPP(&TEHelper::sendNewTaskStatusReport, void(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&))
////        .stubs().will(returnValue());
//
//    // When
//    auto result = ragRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->ruleName, RuleName::RAG_RULE_HANDLER);
//    EXPECT_EQ(result->nextNode, GRAPH_NODE_EndNode);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestDoChain, testDoChainUserInputNoResult) {
//    // Given
//    RAGRuleHandler ragRuleHandler;
//    auto accessCtrl = std::make_shared<AccessCtrl>();
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "team1";
////    accessCtrl->extractInput = []() { return "request"; };
//    accessCtrl->eventType = EventType::USERINPUT;
//    MOCKER(static_cast<bool(*)(const string&, string&, const string&)>(&AisfPlat::Utils::GetEnvVariable))
//        .stubs().with(smirror(ENV_RAG_SAFETYFENCE_FLAG), outBound(string()), smirror(OPEN)).will(returnValue(true));
//    MOCKER_CPP(&SafetyFence::SimilaritySearch, std::vector<std::string>(*)(std::string, std::vector<std::string>))
//        .stubs().will(returnValue(std::vector<std::string>()));
//
//    // When
//    auto result = ragRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestDoChain, testDoChainUserInputRejectedAnswer) {
//    // Given
//    RAGRuleHandler ragRuleHandler;
//    auto accessCtrl = std::make_shared<AccessCtrl>();
//    accessCtrl->header.taskID = "task1";
//    accessCtrl->header.teamEnvironmentID = "team1";
////    accessCtrl->extractInput = []() { return "request"; };
//    accessCtrl->eventType = EventType::USERINPUT;
//    MOCKER(static_cast<bool(*)(const string&, string&, const string&)>(&AisfPlat::Utils::GetEnvVariable))
//        .stubs().with(smirror(ENV_RAG_SAFETYFENCE_FLAG), outBound(string()), smirror(OPEN)).will(returnValue(true));
//    MOCKER_CPP(&SafetyFence::SimilaritySearch, std::vector<std::string>(*)(std::string, std::vector<std::string>))
//        .stubs().will(returnValue(std::vector<std::string>{"0.1", "query", "{\"intentName\": \"REJECTED_ANSWER\", \"data\": {\"content\": \"rejected content\"}}" }));
//    MOCKER_CPP(&SafetyFence::parseIntentName, std::string(*)(const std::string&))
//        .stubs().will(returnValue(std::string("REJECTED_ANSWER")));
//    MOCKER_CPP(&SafetyFence::parseRejectContent, std::string(*)(const std::string&))
//        .stubs().will(returnValue(std::string("rejected content")));
////    MOCKER_CPP(&TEHelper::sendUserOutputMsg, void(*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&, std::string, std::string, int))
////        .stubs().will(returnValue());
//
//    // When
//    auto result = ragRuleHandler.doChain(accessCtrl);
//
//    // Then
//    EXPECT_NE(result, nullptr);
//    EXPECT_EQ(result->ruleName, RuleName::RAG_RULE_HANDLER);
//    EXPECT_EQ(result->nextNode, GRAPH_NODE_EndNode);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
