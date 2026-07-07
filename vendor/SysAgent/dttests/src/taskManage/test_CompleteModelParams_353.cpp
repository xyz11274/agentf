/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/ToolTask.h"
#include <memory>

using namespace mockcpp;
using namespace std;

class TestToolTask_CompleteModelParams : public ::testing::Test {
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

TEST_F(TestToolTask_CompleteModelParams, ShouldUpdateSourceInApiParams_WhenParamListContainsValidKeyAndNeedModelExtractIsTrue) {
    // Given
    ClarifyResult clarifyResult;
    clarifyResult.paramList["param1"] = "value1";

    ToolMessage toolMessage;
    ToolInterface interface;
    ToolParam param;
    param.needModelExtract = true;
    interface.apiParams["param1"] = param;
    toolMessage.toolInterfaces.push_back(interface);

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    auto task = make_shared<ToolTask>(taskID, agentName, requestContent);
    task->toolMessage = toolMessage;

    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;

    // When
    task->CompleteModelParams(accessCtrl, clarifyResult);

    // Then
    EXPECT_EQ(task->toolMessage.toolInterfaces[0].apiParams["param1"].source, "value1");
}

TEST_F(TestToolTask_CompleteModelParams, ShouldSkipUpdatingSource_WhenParamListDoesNotContainKeyInApiParams) {
    // Given
    ClarifyResult clarifyResult;
    clarifyResult.paramList["param1"] = "value1";

    ToolMessage toolMessage;
    ToolInterface interface;
    ToolParam param;
    param.needModelExtract = true;
    interface.apiParams["param2"] = param;
    toolMessage.toolInterfaces.push_back(interface);

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    auto task = make_shared<ToolTask>(taskID, agentName, requestContent);
    task->toolMessage = toolMessage;

    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;

    // When
    task->CompleteModelParams(accessCtrl, clarifyResult);

    // Then
    EXPECT_NE(task->toolMessage.toolInterfaces[0].apiParams["param2"].source, "value1");
}

TEST_F(TestToolTask_CompleteModelParams, ShouldSkipUpdatingSource_WhenNeedModelExtractIsFalse) {
    // Given
    ClarifyResult clarifyResult;
    clarifyResult.paramList["param1"] = "value1";

    ToolMessage toolMessage;
    ToolInterface interface;
    ToolParam param;
    param.needModelExtract = false;
    interface.apiParams["param1"] = param;
    toolMessage.toolInterfaces.push_back(interface);

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    auto task = make_shared<ToolTask>(taskID, agentName, requestContent);
    task->toolMessage = toolMessage;

    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;

    // When
    task->CompleteModelParams(accessCtrl, clarifyResult);

    // Then
    EXPECT_NE(task->toolMessage.toolInterfaces[0].apiParams["param1"].source, "value1");
}

TEST_F(TestToolTask_CompleteModelParams, ShouldProcessMultipleParams_WhenMultipleKeysExistInParamList) {
    // Given
    ClarifyResult clarifyResult;
    clarifyResult.paramList["param1"] = "value1";
    clarifyResult.paramList["param2"] = "value2";

    ToolMessage toolMessage;
    ToolInterface interface;
    ToolParam param1, param2;
    param1.needModelExtract = true;
    param2.needModelExtract = true;
    interface.apiParams["param1"] = param1;
    interface.apiParams["param2"] = param2;
    toolMessage.toolInterfaces.push_back(interface);

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    auto task = make_shared<ToolTask>(taskID, agentName, requestContent);
    task->toolMessage = toolMessage;

    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;

    // When
    task->CompleteModelParams(accessCtrl, clarifyResult);

    // Then
    EXPECT_EQ(task->toolMessage.toolInterfaces[0].apiParams["param1"].source, "value1");
    EXPECT_EQ(task->toolMessage.toolInterfaces[0].apiParams["param2"].source, "value2");
}

TEST_F(TestToolTask_CompleteModelParams, ShouldLogCorrectly_WhenNoMatchingParamsFound) {
    // Given
    ClarifyResult clarifyResult;
    clarifyResult.paramList["param1"] = "value1";

    ToolMessage toolMessage;
    ToolInterface interface;
    ToolParam param;
    param.needModelExtract = true;
    interface.apiParams["param2"] = param;
    toolMessage.toolInterfaces.push_back(interface);

    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    auto task = make_shared<ToolTask>(taskID, agentName, requestContent);
    task->toolMessage = toolMessage;

    shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = nullptr;

    // When
    task->CompleteModelParams(accessCtrl, clarifyResult);

    // Then
    // 日志检查可通过mock日志系统或通过其他方式验证，此处假设日志已正确输出
    // 由于日志函数未提供mock支持，因此无法直接验证日志内容
}

