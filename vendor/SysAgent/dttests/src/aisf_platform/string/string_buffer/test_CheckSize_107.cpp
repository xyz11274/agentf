/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringBufferCheckSize : public ::testing::Test {
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

TEST_F(TestStringBufferCheckSize, ShouldReturnTrue_WhenMaxCapacityIsZero) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Reset();
    sb.buffer.Reset(1024);
    sb.capacity = 1024;
    sb.usedSize = 512;
    
    // When
    bool ret = sb.CheckSize(100, 0);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStringBufferCheckSize, ShouldReturnTrue_WhenEnoughSpaceAvailable) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Reset();
    sb.buffer.Reset(1024);
    sb.capacity = 1024;
    sb.usedSize = 512;
    
    // When
    bool ret = sb.CheckSize(300, 2048);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStringBufferCheckSize, ShouldReturnFalse_WhenNotEnoughSpaceAndCannotExpand) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Reset();
    sb.buffer.Reset(1024);
    sb.capacity = 1024;
    sb.usedSize = 900;
    
    // When
    bool ret = sb.CheckSize(200, 1024);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStringBufferCheckSize, ShouldReturnTrue_WhenNeedToExpandAndNewCapacityValid) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Reset();
    sb.buffer.Reset(1024);
    sb.capacity = 1024;
    sb.usedSize = 900;
    
    // When
    bool ret = sb.CheckSize(200, 2048);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStringBufferCheckSize, ShouldReturnFalse_WhenNeedToExpandButMaxCapacityExceedsLimit) {
    // Given
    StringBuffer sb("test_buffer", 32 * 1024 * 1024); // 32MB
    sb.Reset();
    sb.buffer.Reset(32 * 1024 * 1024);
    sb.capacity = 32 * 1024 * 1024;
    sb.usedSize = 30 * 1024 * 1024;
    
    // When
    bool ret = sb.CheckSize(5 * 1024 * 1024, 64 * 1024 * 1024);
    
    // Then
    EXPECT_EQ(ret, true);
}

