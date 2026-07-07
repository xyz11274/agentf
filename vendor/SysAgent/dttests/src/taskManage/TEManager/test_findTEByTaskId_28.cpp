/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <algorithm>
#include "WorkFlow.h" // workflow
#include "taskManage/AgentManager.h"
#include "taskManage/TEManager.h"
#include "iostream"
#include <iostream>

class TestFindTEByTaskId: public ::testing::Test
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

TEST_F(TestFindTEByTaskId, NoTeamEnvironments) {
    // Given
    TEManager& teManager = TEManager::getInstance();
    teManager.teamIdToTE_.clear();

    // When
    std::shared_ptr<TeamEnvironment> result = teManager.findTEByTaskId("task1");

    // Then
    EXPECT_EQ(result, nullptr);

    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestFindTEByTaskId, TeamEnvironmentWithoutWorkFlow) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    teManager.teamIdToTE_.clear();
//    auto te = std::make_shared<TeamEnvironment>();
//    teManager.addTE(te);
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = teManager.findTEByTaskId("task1");
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestFindTEByTaskId, TaskNotFoundInWorkFlow) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    teManager.teamIdToTE_.clear();
//    auto te = std::make_shared<TeamEnvironment>();
//    te->setWorkFlow(std::make_shared<WorkFlow>());
//    teManager.addTE(te);
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = teManager.findTEByTaskId("task1");
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestFindTEByTaskId, TaskFoundInWorkFlow) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    teManager.teamIdToTE_.clear();
//    auto te = std::make_shared<TeamEnvironment>();
//    auto workFlow = std::make_shared<WorkFlow>();
//    workFlow->addTaskNode(std::make_shared<TaskNode>("task1"));
//    te->setWorkFlow(workFlow);
//    teManager.addTE(te);
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = teManager.findTEByTaskId("task1");
//
//    // Then
//    EXPECT_EQ(result, te);
//
//    // Clean
//    GlobalMockObject::verify();
//}

