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
#include "aisf_platform/common_define.h"
#include "ruleCognition/RuleEntity.h"
#include "config/Config.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace mockcpp;

class TestCommandTaskExec : public ::testing::Test {
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

TEST_F(TestCommandTaskExec, ShouldReturnEarly_WhenIntentIsEmptyAndNotInIntentMap) {

    // Given
    std::string taskID = "test_task_1";
    std::string agentName = "agent1";
    std::string request = "";
    std::string intentPath = "";
    Data data("", "", {});
    CommandTask task(taskID, agentName, request, intentPath, data);

    MOCKER_CPP(&CommandTask::modelExtractParams, void (CommandTask::*)
        (string modelName, string modelUrl, const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, const string& requestContent, string paramsJsonStr))
        .stubs().will(returnValue(0));
    // When
    task.exec(nullptr);
    // Then
    // 无返回值，仅验证日志记录和提前返回
}

//TEST_F(TestCommandTaskExec, ShouldReturnEarly_WhenIntentIsNotEmptyButNotInIntentMap) {
//    // Given
//    CommandTask task("test_task_2", "agent2", "test request", "SerialLModel", Data("", "unknown_intent", {}));
//    MOCKER(&ConfigManger::getInstance).stubs().will(returnObjectList(&ConfigManger::getInstance));
//    MOCKER(&ConfigManger::getIntents).stubs().will(returnValue(std::map<std::string, Intent>{{"other_intent", Intent{false}}}));
//    // When
//    task.exec(nullptr);
//    // Then
//    // 无返回值，仅验证日志记录和提前返回
//}
//
//TEST_F(TestCommandTaskExec, ShouldSendSuccessMsg_WhenIntentHasNoArgs) {
//    // Given
//    CommandTask task("test_task_3", "agent3", "test request", "SerialLModel", Data("", "valid_intent", {}));
//    MOCKER(&ConfigManger::getInstance).stubs().will(returnObjectList(&ConfigManger::getInstance));
//    MOCKER(&ConfigManger::getIntents).stubs().will(returnValue(std::map<std::string, Intent>{{"valid_intent", Intent{true}}}));
//    MOCKER(&TEHelper::sendUserOutputMsg).stubs().will(ignoreReturnValue());
//    MOCKER(&TEHelper::sendNewTaskStatusReport).stubs().will(ignoreReturnValue());
//    // When
//    task.exec(nullptr);
//    // Then
//    // 无返回值，仅验证两个函数调用
//}
//
//TEST_F(TestCommandTaskExec, ShouldCallLLMExtraction_WhenIntentPathIsSerialLModel) {
//    // Given
//    CommandTask task("test_task_4", "agent4", "test request", "SerialLModel", Data("", "valid_intent", {}));
//    MOCKER(&ConfigManger::getInstance).stubs().will(returnObjectList(&ConfigManger::getInstance));
//    MOCKER(&ConfigManger::getIntents).stubs().will(returnValue(std::map<std::string, Intent>{{"valid_intent", Intent{false}}}));
//    MOCKER(&ConfigManger::getModelInfo).stubs().will(returnValue(std::map<std::string, std::string>{{"Qwen3-32B", "http://model.url"}}));
//    MOCKER(&ConfigManger::getIntentToParamsString).stubs().will(returnValue(std::map<std::string, std::string>{}));
//    // When
//    task.exec(nullptr);
//    // Then
//    // 无返回值，仅验证是否调用了extractParamsByLLM
//}
//
//TEST_F(TestCommandTaskExec, ShouldCallLLMExtraction_WhenIntentPathIsParallelLModel) {
//    // Given
//    CommandTask task("test_task_5", "agent5", "test request", "ParallelLModel", Data("", "valid_intent", {}));
//    MOCKER(&ConfigManger::getInstance).stubs().will(returnObjectList(&ConfigManger::getInstance));
//    MOCKER(&ConfigManger::getIntents).stubs().will(returnValue(std::map<std::string, Intent>{{"valid_intent", Intent{false}}}));
//    MOCKER(&ConfigManger::getModelInfo).stubs().will(returnValue(std::map<std::string, std::string>{{"Qwen3-32B", "http://model.url"}}));
//    MOCKER(&ConfigManger::getIntentToParamsString).stubs().will(returnValue(std::map<std::string, std::string>{}));
//    // When
//    task.exec(nullptr);
//    // Then
//    // 无返回值，仅验证是否调用了extractParamsByLLM
//}
//
//TEST_F(TestCommandTaskExec, ShouldSendSuccessMsg_WhenParamsMatchIsTrue) {
//    // Given
//    CommandTask task("test_task_6", "agent6", "test request", "NormalPath", Data("", "valid_intent", {}), true);
//    MOCKER(&TEHelper::sendUserOutputMsg).stubs().will(ignoreReturnValue());
//    MOCKER(&TEHelper::sendNewTaskStatusReport).stubs().will(ignoreReturnValue());
//    // When
//    task.exec(nullptr);
//    // Then
//    // 无返回值，仅验证两个函数调用
//}
//
//TEST_F(TestCommandTaskExec, ShouldCallSmallModelExtraction_WhenParamsMatchIsFalse) {
//    // Given
//    CommandTask task("test_task_7", "agent7", "test request", "NormalPath", Data("", "valid_intent", {}), false);
//    MOCKER(&ConfigManger::getInstance).stubs().will(returnObjectList(&ConfigManger::getInstance));
//    MOCKER(&ConfigManger::getIntents).stubs().will(returnValue(std::map<std::string, Intent>{{"valid_intent", Intent{false}}}));
//    MOCKER(&ConfigManger::getIntentToParamsString).stubs().will(returnValue(std::map<std::string, std::string>{{"valid_intent", "{\"param1\":\"value1\"}"}});
//    MOCKER(&ConfigManger::getModelInfo).stubs().will(returnValue(std::map<std::string, std::string>{{"qwen3-4B", "http://small.model.url"}}));
//    // When
//    task.exec(nullptr);
//    // Then
//    // 无返回值，仅验证是否调用了modelExtractParams
//}
//
