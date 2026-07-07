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

TEST_F(TestATATask, ValidInput) {
    // Given
    std::string taskID1 = "task123";
    auto agent1 = nullptr;
    std::string agentName = "AgentX";
    std::string requestContent = "RequestContent";

    // When
    ATATask atatask(taskID1, agent1, agentName, requestContent);

    // Then
    EXPECT_EQ(atatask.taskID, taskID1);
    EXPECT_EQ(atatask.agent, agent1);
    EXPECT_EQ(atatask.agentName, agentName);
    EXPECT_EQ(atatask.requestContent, requestContent);
    EXPECT_EQ(atatask.taskStatus, TaskStatus::PENDING);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestATATask, EmptyTaskID) {
    // Given
    std::string taskID1 = "";
    auto agent1 = nullptr;
    std::string agentName = "AgentX";
    std::string requestContent = "RequestContent";

    // When
    ATATask atatask(taskID1, agent1, agentName, requestContent);

    // Then
    EXPECT_EQ(atatask.taskID, taskID1);
    EXPECT_EQ(atatask.agent, agent1);
    EXPECT_EQ(atatask.agentName, agentName);
    EXPECT_EQ(atatask.requestContent, requestContent);
    EXPECT_EQ(atatask.taskStatus, TaskStatus::PENDING);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestATATask, NullAgent) {
    // Given
    std::string taskID1 = "task123";
    std::shared_ptr<Agent> agent1 = nullptr;
    std::string agentName = "AgentX";
    std::string requestContent = "RequestContent";

    // When
    ATATask atatask(taskID1, agent1, agentName, requestContent);

    // Then
    EXPECT_EQ(atatask.taskID, taskID1);
    EXPECT_EQ(atatask.agent, agent1);
    EXPECT_EQ(atatask.agentName, agentName);
    EXPECT_EQ(atatask.requestContent, requestContent);
    EXPECT_EQ(atatask.taskStatus, TaskStatus::PENDING);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestATATask, EmptyAgentName) {
    // Given
    std::string taskID1 = "task123";
    auto agent1 = nullptr;
    std::string agentName = "";
    std::string requestContent = "RequestContent";

    // When
    ATATask atatask(taskID1, agent1, agentName, requestContent);

    // Then
    EXPECT_EQ(atatask.taskID, taskID1);
    EXPECT_EQ(atatask.agent, agent1);
    EXPECT_EQ(atatask.agentName, agentName);
    EXPECT_EQ(atatask.requestContent, requestContent);
    EXPECT_EQ(atatask.taskStatus, TaskStatus::PENDING);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestATATask, EmptyRequestContent) {
    // Given
    std::string taskID1 = "task123";
    auto agent1 = nullptr;
    std::string agentName = "AgentX";
    std::string requestContent = "";

    // When
    ATATask atatask(taskID1, agent1, agentName, requestContent);

    // Then
    EXPECT_EQ(atatask.taskID, taskID1);
    EXPECT_EQ(atatask.agent, agent1);
    EXPECT_EQ(atatask.agentName, agentName);
    EXPECT_EQ(atatask.requestContent, requestContent);
    EXPECT_EQ(atatask.taskStatus, TaskStatus::PENDING);

    // Clean
    GlobalMockObject::verify();
}

