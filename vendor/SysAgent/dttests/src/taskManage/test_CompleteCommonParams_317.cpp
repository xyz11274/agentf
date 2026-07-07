/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "taskManage/ToolTask.h"
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
#include "aisf_platform/string/string_utils.h"
#include "config/Config.h"

using namespace mockcpp;
using namespace Json;

class TestToolTaskCompleteCommonParams : public ::testing::Test {
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

TEST_F(TestToolTaskCompleteCommonParams, ShouldCompleteParamsSuccessfully_WhenAccessCtrlHasValidJsonAndToolInterfaces) {
    // Given
    // 构造一个有效的AccessCtrl对象
    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    // 模拟toString()返回合法JSON字符串
    std::string jsonStr = R"({"key1":"value1","key2":"value2"})";
    // 假设toString()返回值可以通过mock实现
    MOCKER(&SYSAGENT::AccessCtrl::toString).stubs().will(returnValue(jsonStr));

    // 构造toolMessage中的toolInterfaces
    ToolMessage toolMessage;
    ToolInterface interfaceItem;
    ToolParam apiParam1;
    apiParam1.source = ""; // 空source，将触发needModelExtract设置
    ToolParam apiParam2;
    apiParam2.source = "key1"; // 非空source，将被替换为params中的值
    interfaceItem.apiParams["param1"] = apiParam1;
    interfaceItem.apiParams["param2"] = apiParam2;
    toolMessage.toolInterfaces.push_back(interfaceItem);

    // 构造payload的Json::Value
    Json::Value payloadValue;
    payloadValue["payloadKey"] = "payloadValue";
    // 模拟accessCtrl->taskAssignment.payload.ToJson(payloadValue)行为
    // 这里我们直接构造payloadValue，因为ToJson是void函数，无法mock
    // 我们假设这个值会被正确解析到params中

    // 构造一个ToolTask实例并设置其toolMessage成员
    std::string taskID = "";
    std::string agentName = "";
    std::string requestContent = "";
    ToolTask toolTask(taskID, agentName, requestContent);
    // 由于toolMessage是私有成员，我们需要通过友元或反射机制访问
    // 为了简化测试，这里我们直接在测试代码中模拟其行为
    // 实际测试中可能需要通过继承或友元方式访问

    // 为方便测试，我们手动设置toolMessage
    // 注意：实际开发中应通过setter或友元函数设置toolMessage
    // 为避免复杂性，此处假设toolMessage已正确设置

    // 模拟CMFrm::Utils::GetEnv("UserToken", "")返回空字符串
//    MOCKER(&CMFrm::Utils::GetEnv).stubs().will(returnValue(std::string("")));

    // When
    // 由于toolMessage是私有成员，我们不能直接访问它
    // 为简化测试，我们假设在测试环境中已经正确设置了toolMessage
    // 实际测试中应通过某种方式注入toolMessage
    // 这里我们假设toolMessage已经被正确设置，且包含上述内容
    // 为了测试目的，我们直接调用函数，假设其内部逻辑能正确处理
    // 由于toolMessage是私有成员，我们只能通过构造函数或友元函数设置
    // 为避免复杂性，我们假设在测试环境中toolMessage已被正确初始化

    // 为测试目的，我们创建一个派生类来访问toolMessage
    class TestableToolTask : public ToolTask {
    public:
        TestableToolTask(std::string& taskID, std::string& agentName, std::string& requestContent)
            : ToolTask(taskID, agentName, requestContent) {}

        void setToolMessage(const ToolMessage& msg) {
            toolMessage = msg;
        }

        bool getNeedExtractParams() const {
            return needExtractParams;
        }
    };
    std::string taskID1 = "";
    std::string agentName1 = "";
    std::string requestContent1 = "";
    TestableToolTask testableToolTask(taskID1, agentName1, requestContent1);
    testableToolTask.setToolMessage(toolMessage);

    // 调用被测函数
    testableToolTask.CompleteCommonParams(accessCtrl);

    // Then
    // 由于toolMessage是私有成员，我们无法直接验证其内容
    // 可以通过检查needExtractParams是否被设置为true来间接验证
    EXPECT_TRUE(testableToolTask.getNeedExtractParams());

    testableToolTask.ExecMcpTool(accessCtrl);
    // Clean
    // 无需清理mock，因为它们在TearDown中会自动验证
}

//// 测试场景2: 工具接口为空的情况
//// UTGen生成测试用例唯一标识符: 47a4c527032a497d8193ff0b88f16363
//TEST_F(TestToolTaskCompleteCommonParams, ShouldHandleEmptyToolInterfaces_WhenNoToolInterfaceDefined) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    std::string jsonStr = R"({"key1":"value1"})";
//    MOCKER(&SYSAGENT::AccessCtrl::toString).stubs().will(returnValue(jsonStr));
//
//    // 构造toolMessage中的toolInterfaces为空
//    ToolMessage toolMessage;
//    // 不添加任何interfaceItem
//
//    class TestableToolTask : public ToolTask {
//    public:
//        TestableToolTask(const std::string& taskID, const std::string& agentName, const std::string& requestContent)
//            : ToolTask(taskID, agentName, requestContent) {}
//
//        void setToolMessage(const ToolMessage& msg) {
//            toolMessage = msg;
//        }
//
//        bool getNeedExtractParams() const {
//            return needExtractParams;
//        }
//    };
//
//    TestableToolTask testableToolTask("", "", "");
//    testableToolTask.setToolMessage(toolMessage);
//
//    // 模拟CMFrm::Utils::GetEnv("UserToken", "")返回空字符串
//    MOCKER(&CMFrm::Utils::GetEnv).stubs().with(eq(std::string("UserToken")), eq(std::string(""))).will(returnValue(""));
//
//    // When
//    testableToolTask.CompleteCommonParams(accessCtrl);
//
//    // Then
//    // 由于toolInterfaces为空，不应发生任何处理，也不应改变needExtractParams
//    EXPECT_FALSE(testableToolTask.getNeedExtractParams());
//
//    // Clean
//    // 无需清理mock，因为它们在TearDown中会自动验证
//}
//
//// 测试场景3: UserToken存在时，应从环境变量中获取并设置source
//// UTGen生成测试用例唯一标识符: 86209e8cb80447a683462111b8e21024
//TEST_F(TestToolTaskCompleteCommonParams, ShouldSetSourceFromEnv_WhenUserTokenExists) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    std::string jsonStr = R"({"key1":"value1"})";
//    MOCKER(&SYSAGENT::AccessCtrl::toString).stubs().will(returnValue(jsonStr));
//
//    ToolMessage toolMessage;
//    ToolInterface interfaceItem;
//    ApiParam apiParam1;
//    apiParam1.source = ""; // 空source，将触发needModelExtract设置
//    apiParam1.name = "param1";
//    interfaceItem.apiParams["param1"] = apiParam1;
//    toolMessage.toolInterfaces.push_back(interfaceItem);
//
//    class TestableToolTask : public ToolTask {
//    public:
//        TestableToolTask(const std::string& taskID, const std::string& agentName, const std::string& requestContent)
//            : ToolTask(taskID, agentName, requestContent) {}
//
//        void setToolMessage(const ToolMessage& msg) {
//            toolMessage = msg;
//        }
//
//        bool getNeedExtractParams() const {
//            return needExtractParams;
//        }
//    };
//
//    TestableToolTask testableToolTask("", "", "");
//    testableToolTask.setToolMessage(toolMessage);
//
//    // 模拟CMFrm::Utils::GetEnv("UserToken", "")返回非空字符串
//    MOCKER(&CMFrm::Utils::GetEnv).stubs().with(eq(std::string("UserToken")), eq(std::string(""))).will(returnValue("token123"));
//
//    // When
//    testableToolTask.CompleteCommonParams(accessCtrl);
//
//    // Then
//    // 验证needExtractParams是否被设置为true
//    EXPECT_TRUE(testableToolTask.getNeedExtractParams());
//
//    // Clean
//    // 无需清理mock，因为它们在TearDown中会自动验证
//}
//
//// 测试场景4: JSON解析失败时，应跳过处理
//// UTGen生成测试用例唯一标识符: b670e26a5c134a6987ea03c667362757
//TEST_F(TestToolTaskCompleteCommonParams, ShouldSkipProcessing_WhenJsonParsingFails) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    // 模拟toString()返回非法JSON字符串
//    std::string invalidJsonStr = R"({invalid json)";
//    MOCKER(&SYSAGENT::AccessCtrl::toString).stubs().will(returnValue(invalidJsonStr));
//
//    // 模拟JsonParser::Parse返回false，表示解析失败
//    MOCKER(&JsonParser::Parse).stubs().with(any(), any()).will(returnValue(false));
//
//    ToolMessage toolMessage;
//    ToolInterface interfaceItem;
//    ApiParam apiParam1;
//    apiParam1.source = "";
//    apiParam1.name = "param1";
//    interfaceItem.apiParams["param1"] = apiParam1;
//    toolMessage.toolInterfaces.push_back(interfaceItem);
//
//    class TestableToolTask : public ToolTask {
//    public:
//        TestableToolTask(const std::string& taskID, const std::string& agentName, const std::string& requestContent)
//            : ToolTask(taskID, agentName, requestContent) {}
//
//        void setToolMessage(const ToolMessage& msg) {
//            toolMessage = msg;
//        }
//
//        bool getNeedExtractParams() const {
//            return needExtractParams;
//        }
//    };
//
//    TestableToolTask testableToolTask("", "", "");
//    testableToolTask.setToolMessage(toolMessage);
//
//    // 模拟CMFrm::Utils::GetEnv("UserToken", "")返回空字符串
//    MOCKER(&CMFrm::Utils::GetEnv).stubs().with(eq(std::string("UserToken")), eq(std::string(""))).will(returnValue(""));
//
//    // When
//    testableToolTask.CompleteCommonParams(accessCtrl);
//
//    // Then
//    // 由于JSON解析失败，不应处理toolInterfaces，needExtractParams应保持为false
//    EXPECT_FALSE(testableToolTask.getNeedExtractParams());
//
//    // Clean
//    // 无需清理mock，因为它们在TearDown中会自动验证
//}
//
//// 测试场景5: 存在多个参数时，应全部处理
//// UTGen生成测试用例唯一标识符: 1b302e98a5794c0dadfbddc372351f7e
//TEST_F(TestToolTaskCompleteCommonParams, ShouldProcessAllApiParams_WhenMultipleParamsExist) {
//    // Given
//    auto accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
//    std::string jsonStr = R"({"key1":"value1","key2":"value2"})";
//    MOCKER(&SYSAGENT::AccessCtrl::toString).stubs().will(returnValue(jsonStr));
//
//    ToolMessage toolMessage;
//    ToolInterface interfaceItem;
//    ApiParam apiParam1;
//    apiParam1.source = ""; // 空source
//    apiParam1.name = "param1";
//    ApiParam apiParam2;
//    apiParam2.source = "key1"; // 非空source
//    apiParam2.name = "param2";
//    ApiParam apiParam3;
//    apiParam3.source = "key2"; // 非空source
//    apiParam3.name = "param3";
//    interfaceItem.apiParams["param1"] = apiParam1;
//    interfaceItem.apiParams["param2"] = apiParam2;
//    interfaceItem.apiParams["param3"] = apiParam3;
//    toolMessage.toolInterfaces.push_back(interfaceItem);
//
//    class TestableToolTask : public ToolTask {
//    public:
//        TestableToolTask(const std::string& taskID, const std::string& agentName, const std::string& requestContent)
//            : ToolTask(taskID, agentName, requestContent) {}
//
//        void setToolMessage(const ToolMessage& msg) {
//            toolMessage = msg;
//        }
//
//        bool getNeedExtractParams() const {
//            return needExtractParams;
//        }
//    };
//
//    TestableToolTask testableToolTask("", "", "");
//    testableToolTask.setToolMessage(toolMessage);
//
//    // 模拟CMFrm::Utils::GetEnv("UserToken", "")返回空字符串
//    MOCKER(&CMFrm::Utils::GetEnv).stubs().with(eq(std::string("UserToken")), eq(std::string(""))).will(returnValue(""));
//
//    // When
//    testableToolTask.CompleteCommonParams(accessCtrl);
//
//    // Then
//    // 验证所有参数都被处理，且needExtractParams被设置为true
//    EXPECT_TRUE(testableToolTask.getNeedExtractParams());
//
//    // Clean
//    // 无需清理mock，因为它们在TearDown中会自动验证
//}

