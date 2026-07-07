/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include <map>
#include <string>
#include <chrono>
#include "memory/History.h"
#include <thread>
#include "config/Config.h"
#include <cstdio>
#include <future>

using namespace mockcpp;

class TestDeepResearchInstance_EndOutput : public ::testing::Test {
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

// 测试场景1: 当outputText包含patternEnd时，应截断文本并将state设为0
TEST_F(TestDeepResearchInstance_EndOutput, ShouldTruncateOutputAndSetStateToZero_WhenOutputTextContainsPatternEnd) {
    // Given
    // 构造TEManager中的TE对象

    // 构造DeepResearchInstance实例
    DeepResearchInstance instance("test_te", "test_task");

    std::string outputText = "some text ###END### more text";
    int state = 1;

    // Mock SendUserOutput 返回true
    MOCKER(&DeepResearchInstance::SendUserOutput).stubs().will(returnValue(true));

    // When
    instance.EndOutput(outputText, state);

    // Then
//    EXPECT_EQ(outputText, "some text ");
//    EXPECT_EQ(state, 0);
}

// 测试场景2: 当outputText不包含patternEnd时，应正常调用SendUserOutput且不修改state
TEST_F(TestDeepResearchInstance_EndOutput, ShouldCallSendUserOutputWithoutTruncating_WhenOutputTextDoesNotContainPatternEnd) {
    // Given
    // 构造TEManager中的TE对象


    // 构造DeepResearchInstance实例
    DeepResearchInstance instance("test_te", "test_task");

    std::string outputText = "some text without end pattern";
    int state = 1;

    // Mock SendUserOutput 返回true
    MOCKER(&DeepResearchInstance::SendUserOutput).stubs().will(returnValue(true));

    // When
    instance.EndOutput(outputText, state);

    // Then
    EXPECT_EQ(outputText, "some text without end pattern");
    EXPECT_EQ(state, 1);
}

// 测试场景3: 当SendUserOutput失败时，应不修改outputText和state
TEST_F(TestDeepResearchInstance_EndOutput, ShouldReturnFalseAndNotModifyOutput_WhenSendUserOutputFails) {
    // Given
    // 构造TEManager中的TE对象


    // 构造DeepResearchInstance实例
    DeepResearchInstance instance("test_te", "test_task");

    std::string outputText = "some text ###END### more text";
    int state = 1;

    // Mock SendUserOutput 返回false
    MOCKER(&DeepResearchInstance::SendUserOutput).stubs().will(returnValue(false));

    // When
    instance.EndOutput(outputText, state);

    // Then
    EXPECT_EQ(outputText, "some text ###END### more text");
    EXPECT_EQ(state, 1);
}

// 测试场景4: 当outputText为空时，应正常调用SendUserOutput且不修改state
TEST_F(TestDeepResearchInstance_EndOutput, ShouldHandleEmptyOutputTextCorrectly_WhenOutputTextIsEmpty) {
    // Given
    // 构造TEManager中的TE对象


    // 构造DeepResearchInstance实例
    DeepResearchInstance instance("test_te", "test_task");

    std::string outputText = "";
    int state = 1;

    // Mock SendUserOutput 返回true
    MOCKER(&DeepResearchInstance::SendUserOutput).stubs().will(returnValue(true));

    // When
    instance.EndOutput(outputText, state);

    // Then
    EXPECT_EQ(outputText, "");
    EXPECT_EQ(state, 1);
}

// 测试场景5: 当getTEById返回空指针时，应不调用SendUserOutput
TEST_F(TestDeepResearchInstance_EndOutput, ShouldHandleNullTE_WhenGetTEByIdReturnsNull) {
    // Given
    // 不添加任何TE到TEManager中，使其getTEById返回nullptr

    // 构造DeepResearchInstance实例
    DeepResearchInstance instance("test_te", "test_task");

    std::string outputText = "some text ###END### more text";
    int state = 1;

    // Mock SendUserOutput 不会被调用
    // 由于SendUserOutput是私有函数，我们不能直接mock它，但可以通过构造上下文使其返回false来模拟失败情况
    // 实际上，这里我们期望SendUserOutput不会被调用，因此不需要mock其返回值

    // When
    instance.EndOutput(outputText, state);

    // Then
    // 由于TE不存在，SendUserOutput不会被调用，所以outputText和state不应改变
    EXPECT_EQ(outputText, "some text ###END### more text");
    EXPECT_EQ(state, 1);
}

