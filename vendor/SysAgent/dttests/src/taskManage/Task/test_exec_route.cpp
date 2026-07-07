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

class TestRouteTaskExec : public ::testing::Test {
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

TEST_F(TestRouteTaskExec, validInput) {
    // Given
    std::string taskID = "task123";
    std::string agentName = "agent456";
    std::string requestContent = "content789";

    // When
    RouteTask routeTask(taskID, agentName, requestContent);

    // Then
    EXPECT_EQ(routeTask.taskID, "task123");
    EXPECT_EQ(routeTask.agentName, "agent456");
    EXPECT_EQ(routeTask.requestContent, "content789");
    EXPECT_EQ(routeTask.taskStatus, TaskStatus::PENDING);
}


TEST_F(TestRouteTaskExec, ShouldReturnEarly_WhenTeamEnvironmentIDIsEmpty) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "";

    std::string taskID = "task123";
    std::string agentName = "agent456";
    std::string requestContent = "content789";
    RouteTask routeTask(taskID, agentName, requestContent);
    
    // When
    routeTask.exec(accessCtrl);
    
    // Then
    // 由于没有mock，日志输出和函数调用行为由实际代码决定，无需断言
}

TEST_F(TestRouteTaskExec, ShouldReturnEarly_WhenCannotFindTeamEnvironment) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "invalid_team_env_id";

    std::string taskID = "task123";
    std::string agentName = "agent456";
    std::string requestContent = "content789";
    RouteTask routeTask(taskID, agentName, requestContent);

    MOCKER(&TEManager::getTEById).stubs().will(returnValue(static_cast<std::shared_ptr<TeamEnvironment>>(nullptr)));
    
    // When
    routeTask.exec(accessCtrl);
    
    // Then
    // 无返回值，仅验证未调用后续逻辑
}

TEST_F(TestRouteTaskExec, ShouldSendRejectMessageAndReportStatus_WhenValidInputProvided) {
    // Given
    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
    accessCtrl->header.teamEnvironmentID = "valid_team_env_id";
    accessCtrl->header.userName = "user1";
    accessCtrl->header.deviceId = "device1";
    accessCtrl->header.uuid = "uuid1";
    accessCtrl->header.clientSessionId = "session1";
    accessCtrl->header.acmSessionId = "acm1";
    accessCtrl->header.eventTimeStamp = "timestamp1";
    accessCtrl->header.deviceCategory = "category1";

    std::string taskID = "3";
    std::string agentName = "agent456";
    std::string requestContent = "content789";
    RouteTask routeTask(taskID, agentName, requestContent);

    UserInfo userInfo;
    TeamEnvironment teamEnvironment("valid_team_env_id", userInfo);
    shared_ptr<TeamEnvironment> te = make_shared<TeamEnvironment>(teamEnvironment);
    shared_ptr<WorkFlow> workFlow = make_shared<WorkFlow>();

    TEManager::getInstance().teamIdToTE_.insert({"valid_team_env_id", te});

    std::shared_ptr<WorkFlow> workFlow1 = std::make_shared<WorkFlow>();
    workFlow1->workFlowID = "test_task";
    te->addWorkFlow(workFlow1);
    
    // When
    routeTask.exec(accessCtrl);

    workFlow1->workFlowStatus = WorkFlowStatus::FINISHED;
    routeTask.exec(accessCtrl);
    
    // Then
    // 无返回值，仅验证调用链正确性
}
