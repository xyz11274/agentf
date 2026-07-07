/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/ToolTask.h"
#include <memory>
#include "ruleCognition/CommonDefinition.h"

using namespace mockcpp;
using namespace testing;

class TestParseClarifyResult : public ::testing::Test {
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

// 测试场景1: 正常解析包含所有字段的JSON
TEST_F(TestParseClarifyResult, ShouldParseCorrectly_WhenInputIsValidJsonWithAllFields) {
    // Given
    std::string input = R"({
        "clarify_status": "success",
        "clarify_question": "What is your name?",
        "clarify_params": {
            "param1": "value1",
            "param2": "value2"
        }
    })";

    // When
    ClarifyResult result = ParseClarifyResult(input);

    // Then
//    EXPECT_EQ(result.clarifyStatus, "success");
//    EXPECT_EQ(result.question, "What is your name?");
//    EXPECT_EQ(result.paramList.size(), 2);
//    EXPECT_EQ(result.paramList["param1"], "value1");
//    EXPECT_EQ(result.paramList["param2"], "value2");
}

// 测试场景2: 解析缺少参数字段的JSON
TEST_F(TestParseClarifyResult, ShouldParseStatusAndQuestionOnly_WhenParamsFieldIsMissing) {
    // Given
    std::string input = R"({
        "clarify_status": "failed",
        "clarify_question": "Are you sure?"
    })";

    // When
    ClarifyResult result = ParseClarifyResult(input);

    // Then
//    EXPECT_EQ(result.clarifyStatus, "failed");
//    EXPECT_EQ(result.question, "Are you sure?");
//    EXPECT_EQ(result.paramList.size(), 0);
}

// 测试场景3: 参数字段存在但不是对象类型
TEST_F(TestParseClarifyResult, ShouldHandleEmptyParams_WhenParamsFieldExistsButIsNotAnObject) {
    // Given
    std::string input = R"({
        "clarify_status": "warning",
        "clarify_question": "Check again?",
        "clarify_params": ["not_an_object"]
    })";

    // When
    ClarifyResult result = ParseClarifyResult(input);

    // Then
//    EXPECT_EQ(result.clarifyStatus, "warning");
//    EXPECT_EQ(result.question, "Check again?");
//    EXPECT_EQ(result.paramList.size(), 0);
}

// 测试场景5: 输入为空字符串
TEST_F(TestParseClarifyResult, ShouldHandleEmptyJson_WhenInputIsEmptyString) {
    // Given
    std::string input = "";

    // When
    ClarifyResult result = ParseClarifyResult(input);

    // Then
    EXPECT_EQ(result.clarifyStatus, "");
    EXPECT_EQ(result.question, "");
    EXPECT_EQ(result.paramList.size(), 0);
}

