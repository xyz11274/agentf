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

using namespace mockcpp;

class TestReplacePlaceholders : public ::testing::Test {
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

// 测试场景1: 替换单个占位符
TEST_F(TestReplacePlaceholders, ShouldReplaceSinglePlaceholder_WhenTemplateHasOnePlaceholder) {
    // Given
    // 无mock需求

    // When
    std::string templateStr = "Hello {name}, welcome to {place}!";
    std::string placeholder = "{name}";
    std::string value = "Alice";
    std::string result = ReplacePlaceholders(templateStr, placeholder, value);

    // Then
    EXPECT_EQ(result, "Hello Alice, welcome to {place}!");
}

// 测试场景2: 替换多个相同占位符
TEST_F(TestReplacePlaceholders, ShouldReplaceMultiplePlaceholders_WhenTemplateHasMultipleSamePlaceholders) {
    // Given
    // 无mock需求

    // When
    std::string templateStr = "{greeting} {name}, {greeting} {name}!";
    std::string placeholder = "{greeting}";
    std::string value = "Hi";
    std::string result = ReplacePlaceholders(templateStr, placeholder, value);

    // Then
    EXPECT_EQ(result, "Hi {name}, Hi {name}!");
}

// 测试场景3: 模板中没有找到占位符
TEST_F(TestReplacePlaceholders, ShouldReturnOriginalString_WhenNoPlaceholderFound) {
    // Given
    // 无mock需求

    // When
    std::string templateStr = "Hello world";
    std::string placeholder = "{name}";
    std::string value = "Alice";
    std::string result = ReplacePlaceholders(templateStr, placeholder, value);

    // Then
    EXPECT_EQ(result, "Hello world");
}

// 测试场景4: 占位符替换为空字符串
TEST_F(TestReplacePlaceholders, ShouldReplaceEmptyValue_WhenPlaceholderReplacedWithEmptyString) {
    // Given
    // 无mock需求

    // When
    std::string templateStr = "Hello {name}!";
    std::string placeholder = "{name}";
    std::string value = "";
    std::string result = ReplacePlaceholders(templateStr, placeholder, value);

    // Then
    EXPECT_EQ(result, "Hello !");
}

// 测试场景5: 占位符包含特殊字符
TEST_F(TestReplacePlaceholders, ShouldHandleSpecialCharactersInPlaceholder_WhenPlaceholderContainsSpecialChars) {
    // Given
    // 无mock需求

    // When
    std::string templateStr = "Price: ${price}$";
    std::string placeholder = "${price}";
    std::string value = "99.99";
    std::string result = ReplacePlaceholders(templateStr, placeholder, value);

    // Then
    EXPECT_EQ(result, "Price: 99.99$");
}

// 测试场景6: 占位符位于字符串末尾
TEST_F(TestReplacePlaceholders, ShouldReplacePlaceholderAtEndOfString_WhenPlaceholderIsAtEnd) {
    // Given
    // 无mock需求

    // When
    std::string templateStr = "Welcome to {place}";
    std::string placeholder = "{place}";
    std::string value = "Wonderland";
    std::string result = ReplacePlaceholders(templateStr, placeholder, value);

    // Then
    EXPECT_EQ(result, "Welcome to Wonderland");
}

// 测试场景7: 占位符位于字符串开头
TEST_F(TestReplacePlaceholders, ShouldReplacePlaceholderAtBeginningOfString_WhenPlaceholderIsAtStart) {
    // Given
    // 无mock需求

    // When
    std::string templateStr = "{greeting} world";
    std::string placeholder = "{greeting}";
    std::string value = "Hello";
    std::string result = ReplacePlaceholders(templateStr, placeholder, value);

    // Then
    EXPECT_EQ(result, "Hello world");
}

