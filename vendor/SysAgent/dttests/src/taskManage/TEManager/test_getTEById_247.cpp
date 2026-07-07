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

class TestGetTEById: public ::testing::Test
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

TEST_F(TestGetTEById, EmptyTeamEnvId) {
    // Given
    std::string teamEnvId = "";

    // When
    std::shared_ptr<TeamEnvironment> result = TEManager::getInstance().getTEById(teamEnvId);

    // Then
    EXPECT_EQ(result, nullptr);

}

TEST_F(TestGetTEById, NonExistentTeamEnvId) {
    // Given
    std::string teamEnvId = "nonexistent";

    // When
    std::shared_ptr<TeamEnvironment> result = TEManager::getInstance().getTEById(teamEnvId);

    // Then
    EXPECT_EQ(result, nullptr);

}

//TEST_F(TestGetTEById, ExistingTeamEnvId) {
//    // Given
//    std::string teamEnvId = "existing";
//    std::shared_ptr<TeamEnvironment> expectedTE = std::make_shared<TeamEnvironment>();
//    TEManager::getInstance().teamIdToTE_[teamEnvId] = expectedTE;
//
//    // When
//    std::shared_ptr<TeamEnvironment> result = TEManager::getInstance().getTEById(teamEnvId);
//
//    // Then
//    EXPECT_EQ(result, expectedTE);
//
//    // Clean
//    TEManager::getInstance().teamIdToTE_.clear();
//}

