/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include <future>
#include <memory>
#include <utility>
#include "framework_common/utils/env_util.h"
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
#include "ruleCognition/CommonDefinition.h"
#include "model/ReactLLMTask.h"

using namespace mockcpp;

class TestATATaskStop : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestATATaskStop, ShouldSendStopRequestAndSetTaskStatusToFinishedByUser_WhenStopIsCalled) {
    // Given
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    access->eventType = SYSAGENT::EventType::TASKASSIGNMENT;

    std::string taskId = "test_task_id";
    std::string agentName = "test_agent_name";
    std::string requestContent = "";
    ATATask task(taskId, nullptr, agentName, requestContent);
    // 设置 taskStatus 初始值
    task.taskStatus = TaskStatus::RUNNING;

    // Mock agent sendRestfulRequest
    std::string ip = "192.168.1.100";
    MOCKER(&Utils::GetBaseIp).stubs().will(returnValue(ip));
    MOCKER_CPP(&Agent::sendRestfulRequest, bool (Agent::*)(const std::string& message) const).stubs().will(returnValue(true));

    // When
    task.stop(access);
    EXPECT_EQ(task.taskStatus, TaskStatus::FINISHED_BY_USER);
    EXPECT_EQ(access->taskStop.header.sysAgentEntity, "");

    access->eventType = SYSAGENT::EventType::USERINPUT;
    task.stop(access);
    EXPECT_EQ(task.taskStatus, TaskStatus::FINISHED_BY_USER);
    EXPECT_EQ(access->userInput.header.sysAgentEntity, "192.168.1.100");

    access->eventType = SYSAGENT::EventType::DATAEXCHANGE;
    task.stop(access);
    EXPECT_EQ(task.taskStatus, TaskStatus::FINISHED_BY_USER);
    EXPECT_EQ(access->dataExchange.header.sysAgentEntity, "192.168.1.100");

    access->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    task.stop(access);
    EXPECT_EQ(task.taskStatus, TaskStatus::FINISHED_BY_USER);
    EXPECT_EQ(access->taskAssignment.header.sysAgentEntity, "192.168.1.100");

    access->eventType = SYSAGENT::EventType::USEROUTPUT;
    task.stop(access);
    EXPECT_EQ(task.taskStatus, TaskStatus::FINISHED_BY_USER);
    EXPECT_EQ(access->userOutput.header.sysAgentEntity, "192.168.1.100");

    access->eventType = SYSAGENT::EventType::TASKSTOP;
    task.stop(access);
    EXPECT_EQ(task.taskStatus, TaskStatus::FINISHED_BY_USER);
    EXPECT_EQ(access->taskStop.header.sysAgentEntity, "192.168.1.100");
}



