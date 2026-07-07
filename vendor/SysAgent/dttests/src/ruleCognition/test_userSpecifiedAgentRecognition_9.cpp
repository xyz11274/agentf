/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "ruleCognition/SpecifyIntentRuleHandler.h"
#include "config/Config.h"
#include "graphManager/GraphData.h"
#include "taskManage/WorkFlow.h"

class TestUserSpecifiedAgentRecognition: public ::testing::Test
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

//TEST_F(TestUserSpecifiedAgentRecognition, UserSpecifiedAgentFound) {
//    // Given
//    MOCKER_CPP(&ConfigManger::getInstance, ConfigManger& (*)()).stubs().will(returnValue(ConfigManger::getInstance()));
//    ConfigManger& configManager = ConfigManger::getInstance();
//    AgentInfo agent1 = {"agent1", {"id1", "id2"}};
//    AgentInfo agent2 = {"agent2", {"id3", "id4"}};
//    configManager.agentList = {agent1, agent2};
//
//    SpecifyIntentRuleHandler specifyIntentRuleHandler;
//
//    // When
//    std::string result = specifyIntentRuleHandler.userSpecifiedAgentRecognition("id2");
//
//    // Then
//    EXPECT_EQ(result, "agent1");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestUserSpecifiedAgentRecognition, UserSpecifiedAgentNotFound) {
//    // Given
//    MOCKER_CPP(&ConfigManger::getInstance, ConfigManger& (*)()).stubs().will(returnValue(ConfigManger::getInstance()));
//    ConfigManger& configManager = ConfigManger::getInstance();
//    AgentInfo agent1 = {"agent1", {"id1", "id2"}};
//    AgentInfo agent2 = {"agent2", {"id3", "id4"}};
//    configManager.agentList = {agent1, agent2};
//
//    SpecifyIntentRuleHandler specifyIntentRuleHandler;
//
//    // When
//    std::string result = specifyIntentRuleHandler.userSpecifiedAgentRecognition("id5");
//
//    // Then
//    EXPECT_EQ(result, "");
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestUserSpecifiedAgentRecognition, EmptyAgentList) {
//    // Given
//    MOCKER_CPP(&ConfigManger::getInstance, ConfigManger& (*)()).stubs().will(returnValue(ConfigManger::getInstance()));
//    ConfigManger& configManager = ConfigManger::getInstance();
//    configManager.agentList.clear();
//
//    SpecifyIntentRuleHandler specifyIntentRuleHandler;
//
//    // When
//    std::string result = specifyIntentRuleHandler.userSpecifiedAgentRecognition("id1");
//
//    // Then
//    EXPECT_EQ(result, "");
//
//    // Clean
//    GlobalMockObject::verify();
//}

