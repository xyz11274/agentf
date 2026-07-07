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

class TestGetSubString : public ::testing::Test {
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

TEST_F(TestGetSubString, ShouldReturnFalse_WhenInputIsNull) {
    // When
    string result;
    bool ret = GetSubString(nullptr, "abc", "def", result);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestGetSubString, ShouldReturnFalse_WhenStartIsNull) {
    // When
    string result;
    bool ret = GetSubString("abcdefg", nullptr, "def", result);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestGetSubString, ShouldReturnFalse_WhenEndIsNull) {
    // When
    string result;
    bool ret = GetSubString("abcdefg", "abc", nullptr, result);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestGetSubString, ShouldReturnFalse_WhenStartNotInData) {
    // When
    string result;
    bool ret = GetSubString("abcdefg", "xyz", "def", result);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, "");
}

TEST_F(TestGetSubString, ShouldReturnFalse_WhenEndNotInData) {
    // When
    string result;
    bool ret = GetSubString("abcdefg", "abc", "xyz", result);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, "");
}

TEST_F(TestGetSubString, ShouldReturnTrueAndSetResult_WhenStartAndEndExistInData) {
    // When
    string result;
    bool ret = GetSubString("abcdefg", "abc", "def", result);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, "");
}

TEST_F(TestGetSubString, ShouldReturnTrueAndSetResult_WhenStartAtBeginningAndEndExists) {
    // When
    string result;
    bool ret = GetSubString("abcdefg", "abc", "def", result);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, "");
}

TEST_F(TestGetSubString, ShouldReturnTrueAndSetResult_WhenStartAndEndAreSame) {
    // When
    string result;
    bool ret = GetSubString("abcabc", "abc", "abc", result);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, "");
}

TEST_F(TestGetSubString, ShouldReturnTrueAndSetResult_WhenSubstringContainsSpecialChars) {
    // When
    string result;
    bool ret = GetSubString("a!@#$%^&*()b", "!@#", "^&*", result);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, "$%");
}

TEST_F(TestGetSubString, ShouldReturnTrueAndSetResult_WhenResultIsNotEmptyAfterClearing) {
    // Given
    string result = "initial";
    // When
    bool ret = GetSubString("abcdefg", "abc", "def", result);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, "");
}

TEST_F(TestGetSubString, ShouldReturnFalse_WhenEndAppearsBeforeStart) {
    // When
    string result;
    bool ret = GetSubString("abdefc", "def", "abc", result);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, "");
}

