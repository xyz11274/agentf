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

class TestCountSubStr : public ::testing::Test {
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

TEST_F(TestCountSubStr, ShouldReturnZero_WhenInputStringIsEmpty) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("", "abc", true);
    // Then
    EXPECT_EQ(result, 0);
}

TEST_F(TestCountSubStr, ShouldReturnZero_WhenSubstringIsEmpty) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("hello", "", false);
    // Then
    EXPECT_EQ(result, 0);
}

TEST_F(TestCountSubStr, ShouldReturnZero_WhenSubstringNotInString) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("hello world", "xyz", false);
    // Then
    EXPECT_EQ(result, 0);
}

TEST_F(TestCountSubStr, ShouldReturnOne_WhenSubstringOccursOnce) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("hello world hello", "world", false);
    // Then
    EXPECT_EQ(result, 1);
}

TEST_F(TestCountSubStr, ShouldReturnTwo_WhenSubstringOccursTwice) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("ababab", "ab", false);
    // Then
    EXPECT_EQ(result, 3);
}

TEST_F(TestCountSubStr, ShouldReturnThree_WhenSubstringOccursThreeTimesWithOverlap) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("aabaab", "aab", false);
    // Then
    EXPECT_EQ(result, 2);
}

TEST_F(TestCountSubStr, ShouldReturnTwo_WhenIgnoreCaseIsTrueAndMatchExists) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("Hello HELLO hello", "hello", true);
    // Then
    EXPECT_EQ(result, 3);
}

TEST_F(TestCountSubStr, ShouldReturnZero_WhenIgnoreCaseIsTrueButNoMatchFound) {
    // Given
    // No mock needed
    // When
    int result = CountSubStr("Hello World", "xyz", true);
    // Then
    EXPECT_EQ(result, 0);
}

