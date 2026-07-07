/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/TeHelper.h"
#include "aisf_platform/common_define.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphManager.h"
#include "graphManager/GraphUtils.h"
#include "mm_customedreact_llm_component.h"
#include "queue/QueueManager.h"
#include <utility>

class TestCreateWorkFlow: public ::testing::Test
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

TEST_F(TestCreateWorkFlow, NullTeamEnvironment) {
    // Given
    std::shared_ptr<TeamEnvironment> te = nullptr;

    // When
    std::string userName = "";
    std::shared_ptr<WorkFlow> workFlow = TEHelper::CreateWorkFlow(te, userName);

    // Then
    EXPECT_EQ(workFlow, nullptr);

}

TEST_F(TestCreateWorkFlow, ValidTeamEnvironment) {
    // Given
    std::string teamId = "";
    UserInfo userInfo = {};
    std::shared_ptr<TeamEnvironment> te = std::make_shared<TeamEnvironment>(teamId, userInfo);

    // When
    std::string userName = "";
    std::shared_ptr<WorkFlow> workFlow = TEHelper::CreateWorkFlow(te, userName);

    // Then
    EXPECT_NE(workFlow, nullptr);
    EXPECT_EQ(true, te->hasWorkFlow());

}

