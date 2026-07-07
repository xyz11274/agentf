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

class TestSplit : public ::testing::Test {
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

TEST_F(TestSplit, ShouldReturnSingleElementVector_WhenDelimiterIsEmptyString) {
    // Given
    std::string s = "hello";
    std::string delimStr = "";
    // When
    auto result = Split(s, delimStr);
    // Then
    ASSERT_EQ(result.size(), 1);
    ASSERT_EQ(result[0], "hello");
}

TEST_F(TestSplit, ShouldReturnEmptyVector_WhenInputStringIsEmptyAndDelimiterIsNotEmpty) {
    // Given
    std::string s = "";
    std::string delimStr = ",";
    // When
    auto result = Split(s, delimStr);
    // Then
    ASSERT_EQ(result.size(), 1);
}

TEST_F(TestSplit, ShouldSplitCorrectly_WhenDelimiterExistsInString) {
    // Given
    std::string s = "a,b,c";
    std::string delimStr = ",";
    // When
    auto result = Split(s, delimStr);
    // Then
    ASSERT_EQ(result.size(), 3);
    ASSERT_EQ(result[0], "a");
    ASSERT_EQ(result[1], "b");
    ASSERT_EQ(result[2], "c");
}

TEST_F(TestSplit, ShouldHandleMultipleConsecutiveDelimiters_WhenTheyExistInString) {
    // Given
    std::string s = "a,,b,c";
    std::string delimStr = ",";
    // When
    auto result = Split(s, delimStr);
    // Then
    ASSERT_EQ(result.size(), 4);
    ASSERT_EQ(result[0], "a");
    ASSERT_EQ(result[1], "");
    ASSERT_EQ(result[2], "b");
    ASSERT_EQ(result[3], "c");
}

TEST_F(TestSplit, ShouldReturnAllElements_WhenDelimiterAtEndOfString) {
    // Given
    std::string s = "a,b,c,";
    std::string delimStr = ",";
    // When
    auto result = Split(s, delimStr);
    // Then
    ASSERT_EQ(result.size(), 4);
    ASSERT_EQ(result[0], "a");
    ASSERT_EQ(result[1], "b");
    ASSERT_EQ(result[2], "c");
    ASSERT_EQ(result[3], "");
}

TEST_F(TestSplit, ShouldReturnOnlyOneElement_WhenDelimiterNotPresentInString) {
    // Given
    std::string s = "abc";
    std::string delimStr = ",";
    // When
    auto result = Split(s, delimStr);
    // Then
    ASSERT_EQ(result.size(), 1);
    ASSERT_EQ(result[0], "abc");
}

TEST_F(TestSplit, ShouldSplitByMultiCharDelimiter_WhenDelimiterIsLongerThanOneCharacter) {
    // Given
    std::string s = "a::b::c";
    std::string delimStr = "::";
    // When
    auto result = Split(s, delimStr);
    // Then
    ASSERT_EQ(result.size(), 3);
    ASSERT_EQ(result[0], "a");
    ASSERT_EQ(result[1], "b");
    ASSERT_EQ(result[2], "c");
}

