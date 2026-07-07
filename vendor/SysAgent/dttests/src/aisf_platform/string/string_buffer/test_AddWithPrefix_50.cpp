/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringBuffer_AddWithPrefix : public ::testing::Test {
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

TEST_F(TestStringBuffer_AddWithPrefix, ShouldReturnZero_WhenDataIsNull) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    // When
    size_t ret = sb.AddWithPrefix(nullptr, 0, nullptr, 0, 1024);
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBuffer_AddWithPrefix, ShouldReturnZero_WhenSizeIsZero) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    // When
    size_t ret = sb.AddWithPrefix("test", 4, "hello", 0, 1024);
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestStringBuffer_AddWithPrefix, ShouldReturnWritePreSize_WhenAddPrefixFailsButAddWithEndCharSucceeds) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    MOCKER(&StringBuffer::Add).stubs().will(returnValue(2));
    // When
    size_t ret = sb.AddWithPrefix("test", 4, "hello", 5, 1024);
    // Then
    EXPECT_EQ(ret, 2);
}

TEST_F(TestStringBuffer_AddWithPrefix, ShouldReturnWritePreSizePlusWriteSize_WhenBothAddOperationsSucceed) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    // When
    size_t ret = sb.AddWithPrefix("test", 4, "hello", 5, 1024);
    // Then
    EXPECT_EQ(ret, 9);
}

TEST_F(TestStringBuffer_AddWithPrefix, ShouldReturnWritePreSize_WhenAddPrefixSucceedsButAddWithEndCharFailsDueToNoSpace) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    MOCKER(&StringBuffer::AddWithEndChar).stubs().will(returnValue(3));
    // When
    size_t ret = sb.AddWithPrefix("test", 4, "hello", 5, 1024);
    // Then
    EXPECT_EQ(ret, 7);
}

TEST_F(TestStringBuffer_AddWithPrefix, ShouldReturnWritePreSizePlusWriteSize_WhenAddPrefixSucceedsAndAddWithEndCharAddsNewline) {
    // Given
    StringBuffer sb("test_buffer", 1024);
    // When
    size_t ret = sb.AddWithPrefix("test", 4, "hello", 5, 1024);
    // Then
    EXPECT_EQ(ret, 9);
}

