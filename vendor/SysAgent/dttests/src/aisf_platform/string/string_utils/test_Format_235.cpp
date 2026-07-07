/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"
#include <climits>
#include <bits/stl_algo.h>

using namespace mockcpp;
using namespace AisfPlat;
using namespace AisfPlat::StringUtils;

class TestFormat : public ::testing::Test {
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

TEST_F(TestFormat, ShouldReturnNegativeOne_WhenBufferIsNull) {
    // Given
    // 无mock
    // When
    const char* format = "%d";
    int result = Format(nullptr, 100, format, 42);
    // Then
    EXPECT_EQ(result, -1);
}

TEST_F(TestFormat, ShouldReturnNegativeOne_WhenFormatIsNull) {
    // Given
    // 无mock
    char buffer[100];
    // When
    int result = Format(buffer, 100, nullptr);
    // Then
    EXPECT_EQ(result, -1);
}

TEST_F(TestFormat, ShouldReturnNegativeOne_WhenBufSizeIsZero) {
    // Given
    // 无mock
    char buffer[100];
    const char* format = "%d";
    // When
    int result = Format(buffer, 0, format, 42);
    // Then
    EXPECT_EQ(result, -1);
}

TEST_F(TestFormat, ShouldReturnLengthOfFormattedString_WhenValidParams) {
    // Given
    // 无mock
    char buffer[100];
    const char* format = "%s %d";
    // When
    int result = Format(buffer, sizeof(buffer), format, "hello", 42);
    // Then
    EXPECT_EQ(result, 8);  // "hello 42" 长度为8
    EXPECT_STREQ(buffer, "hello 42");
}

TEST_F(TestFormat, ShouldTruncateOutput_WhenBufferSizeTooSmall) {
    // Given
    // 无mock
    char buffer[5];  // 只有5个字节的空间
    const char* format = "%s %d";
    // When
    int result = Format(buffer, sizeof(buffer), format, "hello world", 12345);
    // Then
    EXPECT_LE(result, 4);  // 实际长度小于4
    EXPECT_STRNE(buffer, "hello world 12345");  // 应该被截断
}

