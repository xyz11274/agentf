/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "memory/SharedMemory.h"
#include "memory/History.h"
#include "utils/http_client.h"
#include "utils/logger.h"
#include "http_client_factory.h"
#include "model/ReactLLMTask.h"
#include "model/PromptManager.h"
#include "config/Config.h"
#include <vector>
#include <map>
#include <sstream>
#include <future>

using namespace mockcpp;
using namespace testing;

class TestParseJsonToShortMemory : public ::testing::Test {
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

// 测试场景1: ShouldReturnEmptyList_WhenJsonIsNull
TEST_F(TestParseJsonToShortMemory, ShouldReturnEmptyList_WhenJsonIsNull) {
    // Given
    ShortMemory shortMemory;
    shortMemory.userName = "test_user";
    shortMemory.workFlowId = "workflow_123";
    shortMemory.taskId = "task_456";
    shortMemory.createTime = "2023-01-01";

    // When
    std::vector<ShortMemory> result = SharedMemory::ParseJsonToShortMemory("", shortMemory);

    // Then
    EXPECT_EQ(result.size(), 0);
}

// 测试场景2: ShouldReturnEmptyList_WhenJsonIsNotArray
TEST_F(TestParseJsonToShortMemory, ShouldReturnEmptyList_WhenJsonIsNotArray) {
    // Given
    ShortMemory shortMemory;
    shortMemory.userName = "test_user";
    shortMemory.workFlowId = "workflow_123";
    shortMemory.taskId = "task_456";
    shortMemory.createTime = "2023-01-01";

    // When
    std::vector<ShortMemory> result = SharedMemory::ParseJsonToShortMemory("{\"key\":\"value\"}", shortMemory);

    // Then
    EXPECT_EQ(result.size(), 0);
}

// 测试场景3: ShouldReturnOneElementList_WhenJsonContainsOneItem
TEST_F(TestParseJsonToShortMemory, ShouldReturnOneElementList_WhenJsonContainsOneItem) {
    // Given
    ShortMemory shortMemory;
    shortMemory.userName = "test_user";
    shortMemory.workFlowId = "workflow_123";
    shortMemory.taskId = "task_456";
    shortMemory.createTime = "2023-01-01";

    // When
    std::vector<ShortMemory> result = SharedMemory::ParseJsonToShortMemory("[{\"role\":\"user\",\"content\":\"hello\"}]", shortMemory);

    // Then
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].role, "user");
    EXPECT_EQ(result[0].content, "hello");
    EXPECT_EQ(result[0].userName, "test_user");
    EXPECT_EQ(result[0].workFlowId, "workflow_123");
    EXPECT_EQ(result[0].taskId, "task_456");
    EXPECT_EQ(result[0].source, "SystemAgent");
    EXPECT_EQ(result[0].createTime, "2023-01-01.0");
}

// 测试场景4: ShouldReturnMultipleElementsList_WhenJsonContainsMultipleItems
TEST_F(TestParseJsonToShortMemory, ShouldReturnMultipleElementsList_WhenJsonContainsMultipleItems) {
    // Given
    ShortMemory shortMemory;
    shortMemory.userName = "test_user";
    shortMemory.workFlowId = "workflow_123";
    shortMemory.taskId = "task_456";
    shortMemory.createTime = "2023-01-01";

    // When
    std::vector<ShortMemory> result = SharedMemory::ParseJsonToShortMemory("[{\"role\":\"user\",\"content\":\"hello\"},{\"role\":\"assistant\",\"content\":\"hi\"}]", shortMemory);

    // Then
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].role, "user");
    EXPECT_EQ(result[0].content, "hello");
    EXPECT_EQ(result[0].userName, "test_user");
    EXPECT_EQ(result[0].workFlowId, "workflow_123");
    EXPECT_EQ(result[0].taskId, "task_456");
    EXPECT_EQ(result[0].source, "SystemAgent");
    EXPECT_EQ(result[0].createTime, "2023-01-01.0");

    EXPECT_EQ(result[1].role, "assistant");
    EXPECT_EQ(result[1].content, "hi");
    EXPECT_EQ(result[1].userName, "test_user");
    EXPECT_EQ(result[1].workFlowId, "workflow_123");
    EXPECT_EQ(result[1].taskId, "task_456");
    EXPECT_EQ(result[1].source, "SystemAgent");
    EXPECT_EQ(result[1].createTime, "2023-01-01.1");
}

// 测试场景5: ShouldSetCorrectFields_WhenJsonHasMissingFields
TEST_F(TestParseJsonToShortMemory, ShouldSetCorrectFields_WhenJsonHasMissingFields) {
    // Given
    ShortMemory shortMemory;
    shortMemory.userName = "test_user";
    shortMemory.workFlowId = "workflow_123";
    shortMemory.taskId = "task_456";
    shortMemory.createTime = "2023-01-01";

    // When
    std::vector<ShortMemory> result = SharedMemory::ParseJsonToShortMemory("[{\"role\":\"user\"},{\"content\":\"hello\"}]", shortMemory);

    // Then
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].role, "user");
    EXPECT_EQ(result[0].content, "");
    EXPECT_EQ(result[0].userName, "test_user");
    EXPECT_EQ(result[0].workFlowId, "workflow_123");
    EXPECT_EQ(result[0].taskId, "task_456");
    EXPECT_EQ(result[0].source, "SystemAgent");
    EXPECT_EQ(result[0].createTime, "2023-01-01.0");

    EXPECT_EQ(result[1].role, "");
    EXPECT_EQ(result[1].content, "hello");
    EXPECT_EQ(result[1].userName, "test_user");
    EXPECT_EQ(result[1].workFlowId, "workflow_123");
    EXPECT_EQ(result[1].taskId, "task_456");
    EXPECT_EQ(result[1].source, "SystemAgent");
    EXPECT_EQ(result[1].createTime, "2023-01-01.1");
}

// 测试场景6: ShouldSetSourceFieldToSystemAgent_WhenParsingJson
TEST_F(TestParseJsonToShortMemory, ShouldSetSourceFieldToSystemAgent_WhenParsingJson) {
    // Given
    ShortMemory shortMemory;
    shortMemory.userName = "test_user";
    shortMemory.workFlowId = "workflow_123";
    shortMemory.taskId = "task_456";
    shortMemory.createTime = "2023-01-01";

    // When
    std::vector<ShortMemory> result = SharedMemory::ParseJsonToShortMemory("[{\"role\":\"user\",\"content\":\"hello\"}]", shortMemory);

    // Then
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].source, "SystemAgent");
}

