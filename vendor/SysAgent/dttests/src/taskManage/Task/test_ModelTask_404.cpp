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

class TestModelTask: public ::testing::Test
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

TEST_F(TestModelTask, validInput) {
    // Given
    std::string taskID = "task123";
    std::string agentName = "agentA";
    std::string requestContent = "requestContent1";

    // When
    ModelTask modelTask(taskID, agentName, requestContent);

    // Then
    EXPECT_EQ(modelTask.taskID, "task123");
    EXPECT_EQ(modelTask.agentName, "agentA");
    EXPECT_EQ(modelTask.requestContent, "requestContent1");
    EXPECT_EQ(modelTask.taskStatus, TaskStatus::PENDING);

}

TEST_F(TestModelTask, emptyStrings) {
    // Given
    std::string taskID = "";
    std::string agentName = "";
    std::string requestContent = "";

    // When
    ModelTask modelTask(taskID, agentName, requestContent);

    // Then
    EXPECT_EQ(modelTask.taskID, "");
    EXPECT_EQ(modelTask.agentName, "");
    EXPECT_EQ(modelTask.requestContent, "");
    EXPECT_EQ(modelTask.taskStatus, TaskStatus::PENDING);

}

TEST_F(TestModelTask, differentStrings) {
    // Given
    std::string taskID = "task456";
    std::string agentName = "agentB";
    std::string requestContent = "requestContent2";

    // When
    ModelTask modelTask(taskID, agentName, requestContent);

    // Then
    EXPECT_EQ(modelTask.taskID, "task456");
    EXPECT_EQ(modelTask.agentName, "agentB");
    EXPECT_EQ(modelTask.requestContent, "requestContent2");
    EXPECT_EQ(modelTask.taskStatus, TaskStatus::PENDING);

}

