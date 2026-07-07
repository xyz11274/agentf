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

class TestSafeAtoU : public ::testing::Test {
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

TEST_F(TestSafeAtoU, ShouldReturnFalse_WhenDataIsNull) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU(nullptr, result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoU, ShouldReturnFalse_WhenDataIsEmptyString) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU("", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoU, ShouldReturnFalse_WhenStrtoulFailsDueToInvalidChars) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU("123abc", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoU, ShouldReturnFalse_WhenStrtoulOverflowsUIntMax) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU("4294967296", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoU, ShouldReturnFalse_WhenStrtoulReturnsERange) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU("99999999999999999999", result, 10);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

TEST_F(TestSafeAtoU, ShouldReturnValueSuccessfully_WhenValidDecimalStringProvided) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU("12345", result, 10);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, 12345);
}

TEST_F(TestSafeAtoU, ShouldReturnValueSuccessfully_WhenValidHexadecimalStringProvided) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU("0xFF", result, 16);
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(result, 255);
}

TEST_F(TestSafeAtoU, ShouldReturnFalse_WhenRadixIsInvalid) {
    // When
    unsigned result = 0;
    bool ret = SafeAtoU("123", result, 1);
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(result, 0);
}

