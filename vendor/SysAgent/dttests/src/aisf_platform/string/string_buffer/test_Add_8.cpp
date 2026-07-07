/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringBufferAdd : public ::testing::Test {
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

TEST_F(TestStringBufferAdd, ShouldReturnZero_WhenInputDataIsNullptrOrSizeIsZero) {
    // Given
    StringBuffer sb("test_buffer", 100);
    // When
    size_t ret = sb.Add(nullptr, 0, 1000);
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBufferAdd, ShouldReturnZero_WhenBufferIsFull) {
    // Given
    StringBuffer sb("test_buffer", 10);
    sb.Reset(); // Reset to make sure it's empty first
    sb.Add("abcdefghij", 10, 0); // Fill the buffer completely
    // When
    size_t ret = sb.Add("extra", 5, 0);
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBufferAdd, ShouldReturnZero_WhenMemcpyFails) {
    // Given
    StringBuffer sb("test_buffer", 100);
    sb.Reset();
    MOCKER(memcpy_s).stubs().will(returnValue(EINVAL));
    // When
    size_t ret = sb.Add("hello", 5, 0);
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBufferAdd, ShouldReturnCorrectWriteSize_WhenDataAddedSuccessfully) {
    // Given
    StringBuffer sb("test_buffer", 100);
    sb.Reset();
    // When
    size_t ret = sb.Add("hello", 5, 0);
    // Then
    EXPECT_EQ(ret, 5);
    EXPECT_EQ(sb.Size(), 5);
}

TEST_F(TestStringBufferAdd, ShouldReturnZero_WhenMaxCapacityExceededAndNoAutoExpandAllowed) {
    // Given
    StringBuffer sb("test_buffer", 10);
    sb.Reset();
    sb.Add("abcdefgh", 8, 0); // Use up 8 bytes
    // When
    size_t ret = sb.Add("world", 5, 10); // Try to add more than allowed
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBufferAdd, ShouldExpandCapacityAndReturnCorrectSize_WhenMaxCapacityIsSetAndAutoExpandPossible) {
    // Given
    StringBuffer sb("test_buffer", 10);
    sb.Reset();
    sb.Add("abcdefgh", 8, 0); // Use up 8 bytes
    MOCKER(&StringBuffer::ResetCapacity).stubs().will(returnValue(true));
    // When
    size_t ret = sb.Add("world", 5, 20);
    // Then
    EXPECT_EQ(ret, 2);
    EXPECT_EQ(sb.Size(), 10);
}

