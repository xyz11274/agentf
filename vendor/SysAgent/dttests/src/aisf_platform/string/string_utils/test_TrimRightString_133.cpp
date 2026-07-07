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

class TestTrimRightString : public ::testing::Test {
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

TEST_F(TestTrimRightString, ShouldReturnTrimmedString_WhenInputStringEndsWithTrimString) {
    // When
    string s = "hello world!";
    string trimStr = "!";
    string result = TrimRightString(s, trimStr);
    // Then
    EXPECT_EQ(result, "hello world");
}

TEST_F(TestTrimRightString, ShouldReturnOriginalString_WhenInputStringDoesNotEndWithTrimString) {
    // When
    string s = "hello world";
    string trimStr = "!";
    string result = TrimRightString(s, trimStr);
    // Then
    EXPECT_EQ(result, "hello world");
}

TEST_F(TestTrimRightString, ShouldReturnEmptyString_WhenTrimStringEqualsInputString) {
    // When
    string s = "abc";
    string trimStr = "abc";
    string result = TrimRightString(s, trimStr);
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestTrimRightString, ShouldReturnOriginalString_WhenTrimStringIsEmpty) {
    // When
    string s = "hello world";
    string trimStr = "";
    string result = TrimRightString(s, trimStr);
    // Then
    EXPECT_EQ(result, "hello world");
}

TEST_F(TestTrimRightString, ShouldReturnOriginalString_WhenTrimStringIsLongerThanInputString) {
    // When
    string s = "hi";
    string trimStr = "hello world";
    string result = TrimRightString(s, trimStr);
    // Then
    EXPECT_EQ(result, "hi");
}

