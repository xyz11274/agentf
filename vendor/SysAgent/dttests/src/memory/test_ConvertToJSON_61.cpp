/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "memory/SharedMemory.h"
#include <vector>

using namespace mockcpp;
using namespace testing;

class TestConvertToJSON : public ::testing::Test {
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

// 测试场景1: ShouldAppendOneItemToDataArray_WhenContextHasOneMemoryItem
TEST_F(TestConvertToJSON, ShouldAppendOneItemToDataArray_WhenContextHasOneMemoryItem) {
    // Given
    ShortMemory memory;
    memory.userName = "Alice";
    memory.workFlowId = "wf_12345";
    memory.taskId = "task_67890";
    memory.createTime = "2023-04-01T10:00:00Z";
    memory.source = "source_A";
    memory.role = "user";
    memory.content = "This is test content.";

    std::vector<ShortMemory> context;
    context.push_back(memory);

    Json::Value dataArray(Json::arrayValue);

    // When
    SharedMemory::ConvertToJSON(context, dataArray);

    // Then
    ASSERT_EQ(dataArray.size(), 1u);
    EXPECT_EQ(dataArray[0]["userName"].asString(), "Alice");
    EXPECT_EQ(dataArray[0]["workFlowId"].asString(), "wf_12345");
    EXPECT_EQ(dataArray[0]["taskId"].asString(), "task_67890");
    EXPECT_EQ(dataArray[0]["createTime"].asString(), "2023-04-01T10:00:00Z");
    EXPECT_EQ(dataArray[0]["source"].asString(), "source_A");
    EXPECT_EQ(dataArray[0]["role"].asString(), "user");
    EXPECT_EQ(dataArray[0]["content"].asString(), "This is test content.");
}

// 测试场景2: ShouldAppendMultipleItemsToDataArray_WhenContextHasMultipleMemoryItems
TEST_F(TestConvertToJSON, ShouldAppendMultipleItemsToDataArray_WhenContextHasMultipleMemoryItems) {
    // Given
    ShortMemory memory1;
    memory1.userName = "Bob";
    memory1.workFlowId = "wf_11111";
    memory1.taskId = "task_22222";
    memory1.createTime = "2023-04-02T11:00:00Z";
    memory1.source = "source_B";
    memory1.role = "admin";
    memory1.content = "Content from Bob.";

    ShortMemory memory2;
    memory2.userName = "Charlie";
    memory2.workFlowId = "wf_33333";
    memory2.taskId = "task_44444";
    memory2.createTime = "2023-04-03T12:00:00Z";
    memory2.source = "source_C";
    memory2.role = "user";
    memory2.content = "Content from Charlie.";

    std::vector<ShortMemory> context;
    context.push_back(memory1);
    context.push_back(memory2);

    Json::Value dataArray(Json::arrayValue);

    // When
    SharedMemory::ConvertToJSON(context, dataArray);

    // Then
    ASSERT_EQ(dataArray.size(), 2u);
    EXPECT_EQ(dataArray[0]["userName"].asString(), "Bob");
    EXPECT_EQ(dataArray[1]["userName"].asString(), "Charlie");
    EXPECT_EQ(dataArray[0]["content"].asString(), "Content from Bob.");
    EXPECT_EQ(dataArray[1]["content"].asString(), "Content from Charlie.");
}

// 测试场景3: ShouldNotModifyDataArray_WhenContextIsEmpty
TEST_F(TestConvertToJSON, ShouldNotModifyDataArray_WhenContextIsEmpty) {
    // Given
    std::vector<ShortMemory> context;
    Json::Value dataArray(Json::arrayValue);
    dataArray.append(Json::Value("value")); // Pre-fill with one item

    // When
    SharedMemory::ConvertToJSON(context, dataArray);

    // Then
    ASSERT_EQ(dataArray.size(), 1u);
    EXPECT_EQ(dataArray[0].asString(), "value");
}

