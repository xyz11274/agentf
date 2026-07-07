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

class TestSafeAtoi : public ::testing::Test {
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

TEST_F(TestSafeAtoi, ShouldReturnFalse_WhenDataIsNull) {
    // When
    int result = 0;
    bool ret = SafeAtoi(nullptr, result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoi, ShouldReturnFalse_WhenDataIsEmptyString) {
    // When
    int result = 0;
    bool ret = SafeAtoi("", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoi, ShouldReturnFalse_WhenStrtolReturnsOutOfRangeValue) {
    // When
    int result = 0;
    bool ret = SafeAtoi("2147483648", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoi, ShouldReturnFalse_WhenStrtolFailsToParseInput) {
    // When
    int result = 0;
    bool ret = SafeAtoi("123abc", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoi, ShouldReturnFalse_WhenStrtolEncountersERange) {
    // When
    int result = 0;
    bool ret = SafeAtoi("99999999999999999999", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoi, ShouldReturnValueSuccessfully_WhenValidIntegerStringIsProvided) {
    // When
    int result = 0;
    bool ret = SafeAtoi("12345", result, 10);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, 12345);
}

TEST_F(TestSafeAtoi, ShouldReturnFalse_WhenRadixIsInvalid) {
    // When
    int result = 0;
    bool ret = SafeAtoi("123", result, 1);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoi, ShouldReturnTrueWithCorrectResult_WhenHexadecimalStringIsParsed) {
    // When
    int result = 0;
    bool ret = SafeAtoi("0xFF", result, 16);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, 255);
}

