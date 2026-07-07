/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "memory/SharedMemory.h"
#include <vector>
#include <sstream>

using namespace mockcpp;

class TestConvertToVector : public ::testing::Test {
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

// 测试场景1: ShouldConvertSuccessfully_WhenJsonDataHasValidArray
TEST_F(TestConvertToVector, ShouldConvertSuccessfully_WhenJsonDataHasValidArray) {
    // Given
    Json::Value data;
    Json::Value dataArray(Json::arrayValue);
    
    Json::Value item1;
    item1["userName"] = "user1";
    item1["workFlowId"] = "wf1";
    item1["taskId"] = "task1";
    item1["createTime"] = "2023-01-01";
    item1["source"] = "src1";
    item1["role"] = "role1";
    item1["content"] = "content1";
    dataArray.append(item1);

    Json::Value item2;
    item2["userName"] = "user2";
    item2["workFlowId"] = "wf2";
    item2["taskId"] = "task2";
    item2["createTime"] = "2023-01-02";
    item2["source"] = "src2";
    item2["role"] = "role2";
    item2["content"] = "content2";
    dataArray.append(item2);

    data["data"] = dataArray;
    std::vector<ShortMemory> context;

    // When
    SharedMemory::ConvertToVector(data, context);

    // Then
    ASSERT_EQ(context.size(), 2);
    EXPECT_EQ(context[0].userName, "user1");
    EXPECT_EQ(context[0].workFlowId, "wf1");
    EXPECT_EQ(context[0].taskId, "task1");
    EXPECT_EQ(context[0].createTime, "2023-01-01");
    EXPECT_EQ(context[0].source, "src1");
    EXPECT_EQ(context[0].role, "role1");
    EXPECT_EQ(context[0].content, "content1");

    EXPECT_EQ(context[1].userName, "user2");
    EXPECT_EQ(context[1].workFlowId, "wf2");
    EXPECT_EQ(context[1].taskId, "task2");
    EXPECT_EQ(context[1].createTime, "2023-01-02");
    EXPECT_EQ(context[1].source, "src2");
    EXPECT_EQ(context[1].role, "role2");
    EXPECT_EQ(context[1].content, "content2");
}

// 测试场景2: ShouldSkipConversion_WhenJsonDataDoesNotHaveDataMember
TEST_F(TestConvertToVector, ShouldSkipConversion_WhenJsonDataDoesNotHaveDataMember) {
    // Given
    Json::Value data;
    data["otherField"] = "value";
    std::vector<ShortMemory> context;

    // When
    SharedMemory::ConvertToVector(data, context);

    // Then
    ASSERT_EQ(context.size(), 0);
}

// 测试场景3: ShouldSkipConversion_WhenDataMemberIsNotArray
TEST_F(TestConvertToVector, ShouldSkipConversion_WhenDataMemberIsNotArray) {
    // Given
    Json::Value data;
    data["data"] = "notAnArray";
    std::vector<ShortMemory> context;

    // When
    SharedMemory::ConvertToVector(data, context);

    // Then
    ASSERT_EQ(context.size(), 0);
}

// 测试场景4: ShouldHandleMissingFieldsGracefully_WhenSomeFieldsAreMissingInArrayItem
TEST_F(TestConvertToVector, ShouldHandleMissingFieldsGracefully_WhenSomeFieldsAreMissingInArrayItem) {
    // Given
    Json::Value data;
    Json::Value dataArray(Json::arrayValue);

    Json::Value item1;
    item1["userName"] = "user1";
    item1["workFlowId"] = "wf1";
    // 缺少其他字段
    dataArray.append(item1);

    data["data"] = dataArray;
    std::vector<ShortMemory> context;

    // When
    SharedMemory::ConvertToVector(data, context);

    // Then
    ASSERT_EQ(context.size(), 1);
    EXPECT_EQ(context[0].userName, "user1");
    EXPECT_EQ(context[0].workFlowId, "wf1");
    EXPECT_EQ(context[0].taskId, "");
    EXPECT_EQ(context[0].createTime, "");
    EXPECT_EQ(context[0].source, "");
    EXPECT_EQ(context[0].role, "");
    EXPECT_EQ(context[0].content, "");
}

// 测试场景5: ShouldProcessAllItems_WhenJsonDataContainsMultipleArrayElements
TEST_F(TestConvertToVector, ShouldProcessAllItems_WhenJsonDataContainsMultipleArrayElements) {
    // Given
    Json::Value data;
    Json::Value dataArray(Json::arrayValue);

    for (int i = 0; i < 5; ++i) {
        Json::Value item;
        item["userName"] = "user" + std::to_string(i);
        item["workFlowId"] = "wf" + std::to_string(i);
        item["taskId"] = "task" + std::to_string(i);
        item["createTime"] = "2023-01-0" + std::to_string(i+1);
        item["source"] = "src" + std::to_string(i);
        item["role"] = "role" + std::to_string(i);
        item["content"] = "content" + std::to_string(i);
        dataArray.append(item);
    }

    data["data"] = dataArray;
    std::vector<ShortMemory> context;

    // When
    SharedMemory::ConvertToVector(data, context);

    // Then
    ASSERT_EQ(context.size(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(context[i].userName, "user" + std::to_string(i));
        EXPECT_EQ(context[i].workFlowId, "wf" + std::to_string(i));
        EXPECT_EQ(context[i].taskId, "task" + std::to_string(i));
        EXPECT_EQ(context[i].createTime, "2023-01-0" + std::to_string(i+1));
        EXPECT_EQ(context[i].source, "src" + std::to_string(i));
        EXPECT_EQ(context[i].role, "role" + std::to_string(i));
        EXPECT_EQ(context[i].content, "content" + std::to_string(i));
    }
}

