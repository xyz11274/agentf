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

class TestTrimLeftString : public ::testing::Test {
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

TEST_F(TestTrimLeftString, ShouldReturnTrimmedString_WhenInputStringStartsWithTrimString) {
    // When
    string s = "hello world";
    string trimStr = "hello ";
    string result = TrimLeftString(s, trimStr);
    // Then
    EXPECT_EQ(result, "world");
}

TEST_F(TestTrimLeftString, ShouldReturnOriginalString_WhenInputStringDoesNotStartWithTrimString) {
    // When
    string s = "hello world";
    string trimStr = "hi";
    string result = TrimLeftString(s, trimStr);
    // Then
    EXPECT_EQ(result, "hello world");
}

TEST_F(TestTrimLeftString, ShouldReturnEmptyString_WhenTrimStringEqualsInputString) {
    // When
    string s = "test";
    string trimStr = "test";
    string result = TrimLeftString(s, trimStr);
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestTrimLeftString, ShouldReturnOriginalString_WhenTrimStringIsEmpty) {
    // When
    string s = "hello";
    string trimStr = "";
    string result = TrimLeftString(s, trimStr);
    // Then
    EXPECT_EQ(result, "hello");
}

TEST_F(TestTrimLeftString, ShouldReturnSubstring_WhenInputStringContainsTrimStringAtBeginning) {
    // When
    string s = "abcde";
    string trimStr = "abc";
    string result = TrimLeftString(s, trimStr);
    // Then
    EXPECT_EQ(result, "de");
}

