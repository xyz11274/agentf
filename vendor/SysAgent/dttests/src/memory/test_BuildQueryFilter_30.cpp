/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "memory/SharedMemory.h"
#include <vector>

using namespace mockcpp;

class TestBuildQueryFilter : public ::testing::Test {
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

// 测试场景1: ShouldReturnEmptyMap_WhenAllFieldsAreEmpty
TEST_F(TestBuildQueryFilter, ShouldReturnEmptyMap_WhenAllFieldsAreEmpty) {
    // Given
    ShortMemory filter;
    filter.userName = "";
    filter.workFlowId = "";
    filter.taskId = "";
    filter.role = "";
    filter.createTime = "";
    filter.source = "";

    // When
    auto result = SharedMemory::BuildQueryFilter(filter);

    // Then
    EXPECT_EQ(result.size(), 0);
}

// 测试场景2: ShouldReturnFilledMap_WhenSomeFieldsHaveValues
TEST_F(TestBuildQueryFilter, ShouldReturnFilledMap_WhenSomeFieldsHaveValues) {
    // Given
    ShortMemory filter;
    filter.userName = "testUser";
    filter.workFlowId = "wf123";
    filter.taskId = "";
    filter.role = "admin";
    filter.createTime = "";
    filter.source = "";

    // When
    auto result = SharedMemory::BuildQueryFilter(filter);

    // Then
    EXPECT_EQ(result.size(), 3);
    EXPECT_EQ(result["userName"], "testUser");
    EXPECT_EQ(result["workFlowId"], "wf123");
    EXPECT_EQ(result["role"], "admin");
}

// 测试场景3: ShouldReturnCompleteMap_WhenAllFieldsAreSet
TEST_F(TestBuildQueryFilter, ShouldReturnCompleteMap_WhenAllFieldsAreSet) {
    // Given
    ShortMemory filter;
    filter.userName = "user1";
    filter.workFlowId = "workflow1";
    filter.taskId = "task1";
    filter.role = "role1";
    filter.createTime = "2023-01-01";
    filter.source = "source1";

    // When
    auto result = SharedMemory::BuildQueryFilter(filter);

    // Then
    EXPECT_EQ(result.size(), 6);
    EXPECT_EQ(result["userName"], "user1");
    EXPECT_EQ(result["workFlowId"], "workflow1");
    EXPECT_EQ(result["taskId"], "task1");
    EXPECT_EQ(result["role"], "role1");
    EXPECT_EQ(result["createTime"], "2023-01-01");
    EXPECT_EQ(result["source"], "source1");
}

