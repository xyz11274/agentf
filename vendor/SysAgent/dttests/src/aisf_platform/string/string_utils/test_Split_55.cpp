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

TEST_F(TestSplit, ShouldReturnEmptyVector_WhenInputStringIsEmpty) {
    // When
    vector<string> result = Split("", ',');
    // Then
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], "");
}

TEST_F(TestSplit, ShouldReturnSingleElement_WhenNoDelimiterInString) {
    // When
    vector<string> result = Split("hello", ',');
    // Then
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], "hello");
}

TEST_F(TestSplit, ShouldReturnMultipleElements_WhenMultipleDelimitersExist) {
    // When
    vector<string> result = Split("a,b,c,d", ',');
    // Then
    ASSERT_EQ(result.size(), 4);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
    EXPECT_EQ(result[2], "c");
    EXPECT_EQ(result[3], "d");
}

TEST_F(TestSplit, ShouldIncludeEmptyStringAtEnd_WhenTrailingDelimiterExists) {
    // When
    vector<string> result = Split("a,b,c,", ',');
    // Then
    ASSERT_EQ(result.size(), 4);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
    EXPECT_EQ(result[2], "c");
    EXPECT_EQ(result[3], "");
}

TEST_F(TestSplit, ShouldHandleSpecialCharacters_WhenDelimiterIsSpace) {
    // When
    vector<string> result = Split(" a  b c ", ' ');
    // Then
    ASSERT_EQ(result.size(), 6);
    EXPECT_EQ(result[0], "");
    EXPECT_EQ(result[1], "a");
    EXPECT_EQ(result[2], "");
    EXPECT_EQ(result[3], "b");
    EXPECT_EQ(result[4], "c");
    EXPECT_EQ(result[5], "");
}

TEST_F(TestSplit, ShouldReturnOneElement_WhenOnlyDelimiterInString) {
    // When
    vector<string> result = Split(",", ',');
    // Then
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0], "");
    EXPECT_EQ(result[1], "");
}

