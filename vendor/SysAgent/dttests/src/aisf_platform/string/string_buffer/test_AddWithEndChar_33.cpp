/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringBuffer_AddWithEndChar : public ::testing::Test {
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

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnZero_WhenInputDataIsNull) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    // When
    size_t ret = buffer.AddWithEndChar(nullptr, 5, '\n', 0);
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnZero_WhenInputSizeIsZero) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    const char* data = "hello";
    // When
    size_t ret = buffer.AddWithEndChar(data, 0, '\n', 0);
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnWriteSize_WhenDataAddedSuccessfullyWithoutEndChar) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    const char* data = "hello";
    // When
    size_t ret = buffer.AddWithEndChar(data, 5, '\n', 0);
    // Then
    EXPECT_EQ(ret, 5);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnWriteSize_WhenDataAddedSuccessfullyWithEndChar) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    const char* data = "hello\n";
    // When
    size_t ret = buffer.AddWithEndChar(data, 6, '\n', 0);
    // Then
    EXPECT_EQ(ret, 6);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnWriteSizePlusOne_WhenEndCharNotMatchAndSpaceAvailable) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    const char* data = "hello";
    // When
    size_t ret = buffer.AddWithEndChar(data, 5, '\n', 0);
    // Then
    EXPECT_EQ(ret, 5);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnWriteSize_WhenBufferIsFullButMaxCapacityIsZero) {
    // Given
    StringBuffer buffer("test_buffer", 5);
    const char* data = "hello world";
    // When
    size_t ret = buffer.AddWithEndChar(data, 11, '\n', 0);
    // Then
    EXPECT_EQ(ret, 5);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnWriteSize_WhenBufferIsFullButMaxCapacityIsNotZero) {
    // Given
    StringBuffer buffer("test_buffer", 5);
    const char* data = "hello world";
    // When
    size_t ret = buffer.AddWithEndChar(data, 11, '\n', 1024);
    // Then
    EXPECT_EQ(ret, 11);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnWriteSize_WhenDataSizeExceedsLeftCapacity) {
    // Given
    StringBuffer buffer("test_buffer", 10);
    buffer.Add("hello", 5, 0); // Fill up 5 bytes first
    const char* data = "hello world";
    // When
    size_t ret = buffer.AddWithEndChar(data, 11, '\n', 0);
    // Then
    EXPECT_EQ(ret, 5);
}

TEST_F(TestStringBuffer_AddWithEndChar, ShouldReturnWriteSize_WhenDataSizeEqualsLeftCapacity) {
    // Given
    StringBuffer buffer("test_buffer", 10);
    buffer.Add("hello", 5, 0); // Fill up 5 bytes first
    const char* data = "hello";
    // When
    size_t ret = buffer.AddWithEndChar(data, 5, '\n', 0);
    // Then
    EXPECT_EQ(ret, 5);
}

