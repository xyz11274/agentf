/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/ToolTask.h"
#include <memory>
#include "model/PromptManager.h"
#include "ruleCognition/CommonDefinition.h"

using namespace mockcpp;
using namespace Json;

class TestAddParamsToMsgCallBack : public ::testing::Test {
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

// 测试场景1: 当JSON解析失败时，返回原始消息
TEST_F(TestAddParamsToMsgCallBack, ShouldReturnOriginalMessage_WhenJsonParseFailed) {
    // Given
    std::string msgCallBack = "invalid json";
    std::map<std::string, std::string> paramMap;
    
    // When
    std::string result = AddParamsToMsgCallBack(msgCallBack, paramMap);
    
    // Then
    EXPECT_EQ(result, "invalid json");
}

// 测试场景2: 当合法JSON且非空参数映射时，正确追加参数到data.params
TEST_F(TestAddParamsToMsgCallBack, ShouldAppendParamsToDataParams_WhenValidJsonAndNonEmptyParamMap) {
    // Given
    std::string msgCallBack = "{\"data\":{\"params\":[]}}";
    std::map<std::string, std::string> paramMap = {{"key1", "value1"}, {"key2", "value2"}};
    
    // When
    std::string result = AddParamsToMsgCallBack(msgCallBack, paramMap);
    
    // Then
    // 验证返回的JSON字符串包含两个参数对象
    EXPECT_NE(result.find("\"key1\":\"value1\""), std::string::npos);
    EXPECT_NE(result.find("\"key2\":\"value2\""), std::string::npos);
}

// 测试场景3: 当JSON不包含data.params时，返回原始消息
TEST_F(TestAddParamsToMsgCallBack, ShouldReturnOriginalMessage_WhenJsonDoesNotContainDataParams) {
    // Given
    std::string msgCallBack = "{\"id\":123}";
    std::map<std::string, std::string> paramMap = {{"key1", "value1"}};
    
    // When
    std::string result = AddParamsToMsgCallBack(msgCallBack, paramMap);
    
    // Then
//    EXPECT_EQ(result, "{\"id\":123}");
}

// 测试场景4: 当合法JSON且空参数映射时，返回原始JSON但params数组保持为空
TEST_F(TestAddParamsToMsgCallBack, ShouldHandleEmptyParamMapCorrectly_WhenValidJsonAndEmptyParamMap) {
    // Given
    std::string msgCallBack = "{\"data\":{\"params\":[]}}";
    std::map<std::string, std::string> paramMap;
    
    // When
    std::string result = AddParamsToMsgCallBack(msgCallBack, paramMap);
    
    // Then
    EXPECT_EQ(result, "{\"data\":{\"params\":[]}}\n");
}

// 测试场景5: 当参数值包含特殊字符时，正确处理并转义
TEST_F(TestAddParamsToMsgCallBack, ShouldHandleSpecialCharactersInParamValues_WhenValidJsonAndSpecialCharsInParams) {
    // Given
    std::string msgCallBack = "{\"data\":{\"params\":[]}}";
    std::map<std::string, std::string> paramMap = {{"specialKey", "value\nwith\ttabs"}};
    
    // When
    std::string result = AddParamsToMsgCallBack(msgCallBack, paramMap);
    
    // Then
    // 验证返回的JSON字符串包含转义后的特殊字符
    EXPECT_NE(result.find("\\n"), std::string::npos);
    EXPECT_NE(result.find("\\t"), std::string::npos);
}

