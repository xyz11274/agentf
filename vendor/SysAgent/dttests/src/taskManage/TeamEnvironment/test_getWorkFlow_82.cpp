///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include <iostream>
//#include "TeamEnvironment.h"
//#include "taskManage/TeamEnvironment.h"
//#include "mm_customedreact_llm_component.h"
//
//class TestGetWorkFlow: public ::testing::Test
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
//TEST_F(TestGetWorkFlow, workFlowIsNullptr) {
//    // Given
//    TeamEnvironment teamEnvironment("team1", UserInfo());
//    teamEnvironment.workFlow_ = nullptr;
//
//    // When
//    std::shared_ptr<WorkFlow> result = teamEnvironment.getWorkFlow("task1");
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}
//
//TEST_F(TestGetWorkFlow, taskIDNotFound) {
//    // Given
//    TeamEnvironment teamEnvironment("team1", UserInfo());
//    teamEnvironment.workFlow_ = std::make_shared<WorkFlow>();
//    teamEnvironment.workFlow_->workFlowID = "workflow1";
//
//    // When
//    std::shared_ptr<WorkFlow> result = teamEnvironment.getWorkFlow("task2");
//
//    // Then
//    EXPECT_EQ(result, nullptr);
//
//}
//
//TEST_F(TestGetWorkFlow, taskIDFound) {
//    // Given
//    TeamEnvironment teamEnvironment("team1", UserInfo());
//    teamEnvironment.workFlow_ = std::make_shared<WorkFlow>();
//    teamEnvironment.workFlow_->workFlowID = "workflow1";
//
//    // When
//    std::shared_ptr<WorkFlow> result = teamEnvironment.getWorkFlow("workflow1");
//
//    // Then
//    EXPECT_EQ(result, teamEnvironment.workFlow_);
//
//}
//
