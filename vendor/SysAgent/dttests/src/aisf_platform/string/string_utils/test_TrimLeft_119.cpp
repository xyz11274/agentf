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

class TestTrimLeft : public ::testing::Test {
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

TEST_F(TestTrimLeft, ShouldReturnEmptyString_WhenInputIsEmpty) {
    // When
    std::string input = "";
    std::string result = TrimLeft(input);
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestTrimLeft, ShouldReturnOriginalString_WhenNoLeadingSpaces) {
    // When
    std::string input = "hello world";
    std::string result = TrimLeft(input);
    // Then
    EXPECT_EQ(result, "hello world");
}

TEST_F(TestTrimLeft, ShouldRemoveAllLeadingSpaces_WhenOnlySpaces) {
    // When
    std::string input = "   ";
    std::string result = TrimLeft(input);
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestTrimLeft, ShouldRemoveOnlyLeadingSpaces_WhenMixedWithSpaces) {
    // When
    std::string input = "   hello world   ";
    std::string result = TrimLeft(input);
    // Then
    EXPECT_EQ(result, "hello world   ");
}

TEST_F(TestTrimLeft, ShouldHandleTabAndNewline_WhenLeadingTabsAndNewlinesExist) {
    // When
    std::string input = "\t\n hello";
    std::string result = TrimLeft(input);
    // Then
    EXPECT_EQ(result, "hello");
}

