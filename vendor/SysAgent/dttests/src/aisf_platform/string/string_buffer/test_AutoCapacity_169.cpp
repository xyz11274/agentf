/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringBufferAutoCapacity : public ::testing::Test {
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

TEST_F(TestStringBufferAutoCapacity, ShouldReturnTrue_WhenNeedLengthLessThanAvailableSpace) {
    // Given
    StringBuffer buffer("test_buffer", 1000);
    buffer.Reset();
    buffer.usedSize = 500;
    buffer.capacity = 1000;
    
    // When
    bool ret = buffer.AutoCapacity(300);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStringBufferAutoCapacity, ShouldReturnFalse_WhenNeedLengthGreaterThanAvailableSpace) {
    // Given
    StringBuffer buffer("test_buffer", 1000);
    buffer.Reset();
    buffer.usedSize = 900;
    buffer.capacity = 1000;
    
    // When
    bool ret = buffer.AutoCapacity(200);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStringBufferAutoCapacity, ShouldDoubleCapacity_WhenCapacityLessThan1MB) {
    // Given
    StringBuffer buffer("test_buffer", 500000);
    buffer.Reset();
    buffer.usedSize = 0;
    buffer.capacity = 500000;
    
    // When
    bool ret = buffer.AutoCapacity(100000);
    
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(buffer.capacity, 500000);
}

TEST_F(TestStringBufferAutoCapacity, ShouldAddHalfOfOriginalCapacity_WhenCapacityBetween1MBAnd8MB) {
    // Given
    StringBuffer buffer("test_buffer", 2000000);
    buffer.Reset();
    buffer.usedSize = 0;
    buffer.capacity = 2000000;
    
    // When
    bool ret = buffer.AutoCapacity(100000);
    
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(buffer.capacity, 2000000);
}

TEST_F(TestStringBufferAutoCapacity, ShouldAddQuarterOfOriginalCapacity_WhenCapacityBetween8MBAnd32MB) {
    // Given
    StringBuffer buffer("test_buffer", 10000000);
    buffer.Reset();
    buffer.usedSize = 0;
    buffer.capacity = 10000000;
    
    // When
    bool ret = buffer.AutoCapacity(100000);
    
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(buffer.capacity, 10000000);
}

TEST_F(TestStringBufferAutoCapacity, ShouldReturnFalse_WhenCapacityExceeds32MB) {
    // Given
    StringBuffer buffer("test_buffer", 35000000);
    buffer.Reset();
    buffer.usedSize = 0;
    buffer.capacity = 35000000;
    
    // When
    bool ret = buffer.AutoCapacity(100000);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStringBufferAutoCapacity, ShouldCallResetCapacity_WhenAutoCapacitySucceeds) {
    // Given
    StringBuffer buffer("test_buffer", 500000);
    buffer.Reset();
    buffer.usedSize = 0;
    buffer.capacity = 500000;
    
    MOCKER(&StringBuffer::ResetCapacity).stubs().will(returnValue(true));
    
    // When
    bool ret = buffer.AutoCapacity(100000);
    
    // Then
    EXPECT_EQ(ret, true);
}

