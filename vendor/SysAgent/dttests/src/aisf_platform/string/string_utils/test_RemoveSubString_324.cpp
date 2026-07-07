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

class TestRemoveSubString : public ::testing::Test {
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

TEST_F(TestRemoveSubString, ShouldReturnFalse_WhenDataIsNull) {
    // When
    bool ret = RemoveSubString(nullptr, 10, "abc", "xyz");
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestRemoveSubString, ShouldReturnFalse_WhenStartIsNull) {
    // When
    char data[] = "abcdefg";
    bool ret = RemoveSubString(data, 7, nullptr, "xyz");
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestRemoveSubString, ShouldReturnFalse_WhenEndIsNull) {
    // When
    char data[] = "abcdefg";
    bool ret = RemoveSubString(data, 7, "abc", nullptr);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestRemoveSubString, ShouldReturnFalse_WhenDataLenIsZero) {
    // When
    char data[] = "abcdefg";
    bool ret = RemoveSubString(data, 0, "abc", "xyz");
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestRemoveSubString, ShouldReturnFalse_WhenStartNotInData) {
    // When
    char data[] = "abcdefg";
    bool ret = RemoveSubString(data, 7, "xyz", "uvw");
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestRemoveSubString, ShouldReturnFalse_WhenEndNotInDataAfterStart) {
    // When
    char data[] = "abcde";
    bool ret = RemoveSubString(data, 5, "abc", "xyz");
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestRemoveSubString, ShouldReturnTrueAndModifyData_WhenSubstringFoundAndRemoved) {
    // When
    char data[] = "hello[world]test";
    bool ret = RemoveSubString(data, 17, "[", "]");
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_STREQ(data, "hellotest");
}

TEST_F(TestRemoveSubString, ShouldReturnFalse_WhenMemmoveSFailed) {
    // Given
    MOCKER(memmove_s).stubs().will(returnValue(EINVAL));
    // When
    char data[] = "hello[world]test";
    bool ret = RemoveSubString(data, 17, "[", "]");
    // Then
    EXPECT_EQ(ret, false);
}

