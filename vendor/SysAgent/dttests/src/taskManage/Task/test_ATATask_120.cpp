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

class TestATATask: public ::testing::Test
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

TEST_F(TestATATask, ut_with_data) {
    // Given
    std::string taskID = "task123";
    std::shared_ptr<Agent> agent = nullptr;
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    Data data("category", "intent", {{"key1", "value1"}, {"key2", "value2"}});

    // When
    ATATask atatask(taskID, agent, agentName, requestContent, data);

    // Then
    EXPECT_EQ(atatask.taskID, taskID);
    EXPECT_EQ(atatask.agent, agent);
    EXPECT_EQ(atatask.agentName, agentName);
    EXPECT_EQ(atatask.requestContent, requestContent);
    EXPECT_EQ(atatask.taskStatus, TaskStatus::PENDING);
    EXPECT_EQ(atatask.data.category, data.category);
    EXPECT_EQ(atatask.data.intent, data.intent);
    EXPECT_EQ(atatask.data.params, data.params);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestATATask, ut_without_data) {
    // Given
    std::string taskID = "task123";
    std::shared_ptr<Agent> agent = nullptr;
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    Data data;

    // When
    ATATask atatask(taskID, agent, agentName, requestContent, data);

    // Then
    EXPECT_EQ(atatask.taskID, taskID);
    EXPECT_EQ(atatask.agent, agent);
    EXPECT_EQ(atatask.agentName, agentName);
    EXPECT_EQ(atatask.requestContent, requestContent);
    EXPECT_EQ(atatask.taskStatus, TaskStatus::PENDING);
    EXPECT_EQ(atatask.data.category, data.category);
    EXPECT_EQ(atatask.data.intent, data.intent);
    EXPECT_EQ(atatask.data.params, data.params);

    // Clean
    GlobalMockObject::verify();
}

