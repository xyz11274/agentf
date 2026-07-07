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

class TestTrimRight : public ::testing::Test {
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

TEST_F(TestTrimRight, ShouldReturnEmptyString_WhenInputIsEmpty) {
    // When
    std::string input = "";
    std::string result = TrimRight(input);
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestTrimRight, ShouldReturnOriginalString_WhenNoTrailingSpaces) {
    // When
    std::string input = "hello";
    std::string result = TrimRight(input);
    // Then
    EXPECT_EQ(result, "hello");
}

TEST_F(TestTrimRight, ShouldRemoveTrailingSpaces_WhenInputHasOnlySpaces) {
    // When
    std::string input = "   ";
    std::string result = TrimRight(input);
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestTrimRight, ShouldTrimTrailingSpaces_WhenInputHasMixedCharactersAndSpaces) {
    // When
    std::string input = "hello world   ";
    std::string result = TrimRight(input);
    // Then
    EXPECT_EQ(result, "hello world");
}

TEST_F(TestTrimRight, ShouldTrimMultipleTypesOfWhitespace_WhenInputContainsVariousWhitespaces) {
    // When
    std::string input = "test\n\t  ";
    std::string result = TrimRight(input);
    // Then
    EXPECT_EQ(result, "test");
}

TEST_F(TestTrimRight, ShouldNotModifyLeadingSpaces_WhenInputHasLeadingAndTrailingSpaces) {
    // When
    std::string input = "  hello world  ";
    std::string result = TrimRight(input);
    // Then
    EXPECT_EQ(result, "  hello world");
}

