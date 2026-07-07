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

class TestStop: public ::testing::Test
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

TEST_F(TestStop, ut_name) {
    // Given
    std::string taskID = "task123";
    std::string agentName = "agent1";
    std::string requestContent = "request";
    RouteTask routeTask(taskID, agentName, requestContent);
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();

    // When
    routeTask.stop(accessCtrl);

    // Then
    EXPECT_EQ(routeTask.taskStatus, TaskStatus::FINISHED);

    // Clean
    GlobalMockObject::verify();
}

