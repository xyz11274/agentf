/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/Task.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "config/Config.h"
#include "memory/Location.h"
#include "mm_customedreact_llm_component.h"
#include "model/PromptManager.h"
#include "queue/QueueManager.h"
#include "taskManage/QAAgentCallback.h"
#include "taskManage/TeHelper.h"
#include "taskManage/WorkFlow.h"
#include <future>
#include <memory>
#include <utility>
#include "ruleCognition/CommonDefinition.h"
#include "model/ReactLLMTask.h"

class TestGetAssignedAgent: public ::testing::Test
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

TEST_F(TestGetAssignedAgent, ut_assigned_agent) {
    // Given
    std::string taskID = "task1";
    std::shared_ptr<Agent> agent = nullptr;
    std::string agentName = "Agent1";
    std::string requestContent = "Request1";
    ATATask atatask(taskID, agent, agentName, requestContent);

    // When
    std::shared_ptr<Agent> result = atatask.getAssignedAgent();

    // Then
    EXPECT_EQ(result, agent);

}

TEST_F(TestGetAssignedAgent, ut_no_assigned_agent) {
    // Given
    std::string taskID = "task1";
    std::shared_ptr<Agent> agent = nullptr;
    std::string agentName = "Agent1";
    std::string requestContent = "Request1";
    ATATask atatask(taskID, agent, agentName, requestContent);

    // When
    std::shared_ptr<Agent> result = atatask.getAssignedAgent();

    // Then
    EXPECT_EQ(result, nullptr);

}

