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

class TestGetSharedMemoryQuantity : public ::testing::Test {
protected:
    virtual void SetUp()
    {
        // 初始化共享内存相关静态成员变量
        SharedMemory::totalSize_ = 1000;
        SharedMemory::summarySize_ = 100;
        SharedMemory::roundSize_ = 10;
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
        // 清理静态成员变量状态
        SharedMemory::totalSize_ = 0;
        SharedMemory::summarySize_ = 0;
        SharedMemory::roundSize_ = 0;
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

// 测试场景1: 当传入的 shared_ptr<vector<ShortMemory>> 为空时，函数应返回默认值 {0, 0}
TEST_F(TestGetSharedMemoryQuantity, ShouldReturnDefaultValues_WhenInputIsNullptr) {
    // Given
    std::shared_ptr<std::vector<ShortMemory>> memory = nullptr;
    // When
    auto result = SharedMemory::GetSharedMemoryQuantity(memory);
    // Then
    ASSERT_EQ(result.size(), 2);
    ASSERT_EQ(result[0], 0);
    ASSERT_EQ(result[1], 0);
}

// 测试场景2: 当传入的 vector 为空时，函数应返回默认值 {0, 0}
TEST_F(TestGetSharedMemoryQuantity, ShouldReturnDefaultValues_WhenMemoryIsEmpty) {
    // Given
    std::vector<ShortMemory> emptyVec;
    std::shared_ptr<std::vector<ShortMemory>> memory = std::make_shared<std::vector<ShortMemory>>(emptyVec);
    // When
    auto result = SharedMemory::GetSharedMemoryQuantity(memory);
    // Then
    ASSERT_EQ(result.size(), 2);
    ASSERT_EQ(result[0], 0);
    ASSERT_EQ(result[1], 0);
}

// 测试场景3: 当 vector 中元素 role 不是 "user" 时，不会计入计算，返回值应为 {0, 0}
TEST_F(TestGetSharedMemoryQuantity, ShouldReturnZero_WhenRoleIsNotUser) {
    // Given
    ShortMemory item;
    item.role = "assistant";
    item.source = "someSource";
    std::vector<ShortMemory> vec = {item};
    std::shared_ptr<std::vector<ShortMemory>> memory = std::make_shared<std::vector<ShortMemory>>(vec);
    // When
    auto result = SharedMemory::GetSharedMemoryQuantity(memory);
    // Then
    ASSERT_EQ(result.size(), 2);
    ASSERT_EQ(result[0], 0);
    ASSERT_EQ(result[1], 0);
}

// 测试场景4: 当 vector 中元素 role 是 "user" 且 source 是 "SystemAgent" 时，res 应增加 summarySize_
TEST_F(TestGetSharedMemoryQuantity, ShouldAddSummarySize_WhenSourceIsSystemAgent) {
    // Given
    ShortMemory item;
    item.role = "user";
    item.source = "SystemAgent";
    std::vector<ShortMemory> vec = {item};
    std::shared_ptr<std::vector<ShortMemory>> memory = std::make_shared<std::vector<ShortMemory>>(vec);
    // When
    auto result = SharedMemory::GetSharedMemoryQuantity(memory);
    // Then
    ASSERT_EQ(result.size(), 2);
    ASSERT_EQ(result[0], SharedMemory::summarySize_);
    ASSERT_EQ(result[1], 0);
}

// 测试场景5: 当 vector 中元素 role 是 "user" 且 source 不是 "SystemAgent" 时，res 增加 1，canUpdate 增加 1
TEST_F(TestGetSharedMemoryQuantity, ShouldIncrementResAndCanUpdate_WhenSourceIsNotSystemAgent) {
    // Given
    ShortMemory item;
    item.role = "user";
    item.source = "someOtherSource";
    std::vector<ShortMemory> vec = {item};
    std::shared_ptr<std::vector<ShortMemory>> memory = std::make_shared<std::vector<ShortMemory>>(vec);
    // When
    auto result = SharedMemory::GetSharedMemoryQuantity(memory);
    // Then
    ASSERT_EQ(result.size(), 2);
    ASSERT_EQ(result[0], 1);
    ASSERT_EQ(result[1], 1);
}
