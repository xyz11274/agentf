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

class TestSetAgent: public ::testing::Test
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

TEST_F(TestSetAgent, validAgent) {
    // Given
    std::shared_ptr<Agent> validAgent = std::make_shared<Agent>("", "", "", "", "");
    std::string taskID = "task1";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask ataTask(taskID, nullptr, agentName, requestContent);

    // When
    bool result = ataTask.setAgent(validAgent);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(ataTask.agent, validAgent);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestSetAgent, nullAgent) {
    // Given
    std::shared_ptr<Agent> nullAgent = nullptr;
    std::string taskID = "task1";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask ataTask(taskID, nullptr, agentName, requestContent);
    // When
    bool result = ataTask.setAgent(nullAgent);

    // Then
    EXPECT_EQ(result, false);
    EXPECT_EQ(ataTask.agent, nullptr);

    // Clean
    GlobalMockObject::verify();
}

