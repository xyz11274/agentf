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

class TestGetAllTEs: public ::testing::Test
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

TEST_F(TestGetAllTEs, getAllTEs_EmptyMap) {
    // Given
    TEManager& teManager = TEManager::getInstance();

    // When
    std::vector<std::shared_ptr<TeamEnvironment>> result = teManager.getAllTEs();

    // Then
    EXPECT_EQ(result.size(), 0);

    // Clean
    GlobalMockObject::verify();
}

//TEST_F(TestGetAllTEs, getAllTEs_OneElement) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    auto te = std::make_shared<TeamEnvironment>();
//    teManager.teamIdToTE_["team1"] = te;
//
//    // When
//    std::vector<std::shared_ptr<TeamEnvironment>> result = teManager.getAllTEs();
//
//    // Then
//    EXPECT_EQ(result.size(), 1);
//    EXPECT_EQ(result[0], te);
//
//    // Clean
//    GlobalMockObject::verify();
//    teManager.teamIdToTE_.clear();
//}

//TEST_F(TestGetAllTEs, getAllTEs_MultipleElements) {
//    // Given
//    TEManager& teManager = TEManager::getInstance();
//    auto te1 = std::make_shared<TeamEnvironment>();
//    auto te2 = std::make_shared<TeamEnvironment>();
//    teManager.teamIdToTE_["team1"] = te1;
//    teManager.teamIdToTE_["team2"] = te2;
//
//    // When
//    std::vector<std::shared_ptr<TeamEnvironment>> result = teManager.getAllTEs();
//
//    // Then
//    EXPECT_EQ(result.size(), 2);
//    EXPECT_EQ(result[0], te1);
//    EXPECT_EQ(result[1], te2);
//
//    // Clean
//    GlobalMockObject::verify();
//    teManager.teamIdToTE_.clear();
//}

