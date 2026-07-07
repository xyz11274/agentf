/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringBufferResetCapacity : public ::testing::Test {
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

TEST_F(TestStringBufferResetCapacity, ShouldReturnTrue_WhenNewCapacityLessThanOrEqualToCurrentCapacity) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Reset(); // Ensure usedSize is 0 for this test case
    size_t newCapacity = 512;

    // When
    bool ret = sb.ResetCapacity(newCapacity);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(sb.Capacity(), 1024); // capacity should not change
}

TEST_F(TestStringBufferResetCapacity, ShouldReturnFalse_WhenGetOldDataFails) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Add("hello", 5); // Set usedSize to 5
    MOCKER(&SafeString::Get).stubs().will(returnValue(false));

    size_t newCapacity = 256;

    // When
    bool ret = sb.ResetCapacity(newCapacity);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(sb.Capacity(), 1024); // capacity should not change
}

TEST_F(TestStringBufferResetCapacity, ShouldReturnFalse_WhenInitialFails) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Add("hello", 5); // Set usedSize to 5
    MOCKER(&StringBuffer::Initial).stubs().will(returnValue(false));

    size_t newCapacity = 256;

    // When
    bool ret = sb.ResetCapacity(newCapacity);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(sb.Capacity(), 1024); // capacity should not change
}

TEST_F(TestStringBufferResetCapacity, ShouldUpdateCapacityAndCopyOldData_WhenUsedSizeIsNonZero) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Add("hello world", 11); // Set usedSize to 11
    size_t newCapacity = 1024;

    // When
    bool ret = sb.ResetCapacity(newCapacity);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(sb.Capacity(), newCapacity);
    EXPECT_EQ(sb.Size(), 11); // Should have copied old data
}

TEST_F(TestStringBufferResetCapacity, ShouldReturnTrue_WhenUsedSizeIsZero) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Reset(); // Ensure usedSize is 0
    size_t newCapacity = 1024;

    // When
    bool ret = sb.ResetCapacity(newCapacity);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(sb.Capacity(), newCapacity);
    EXPECT_EQ(sb.Size(), 0); // Should remain empty
}

TEST_F(TestStringBufferResetCapacity, ShouldReturnFalse_WhenMemcpySFails) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    sb.Add("hello", 5); // Set usedSize to 5
    MOCKER(memcpy_s).stubs().will(returnValue(EINVAL));

    size_t newCapacity = 256;

    // When
    bool ret = sb.ResetCapacity(newCapacity);

    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(sb.Capacity(), 1024); // capacity should not change
}

