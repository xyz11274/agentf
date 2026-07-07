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

class TestFormat : public ::testing::Test {
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

TEST_F(TestFormat, ShouldReturnEmptyString_WhenFormatIsNullptr) {
    // When
    string ret = Format(100, nullptr);
    // Then
    EXPECT_EQ(ret, "");
}

TEST_F(TestFormat, ShouldReturnEmptyString_WhenBufSizeIsZero) {
    // When
    string ret = Format(0, "test %s", "value");
    // Then
    EXPECT_EQ(ret, "");
}

TEST_F(TestFormat, ShouldReturnFormattedString_WhenValidInputProvided) {
    // When
    string ret = Format(100, "Hello %s!", "World");
    // Then
    EXPECT_EQ(ret, "Hello World!");
}

TEST_F(TestFormat, ShouldLogError_WhenVsnprintfSReturnsNegativeOne) {
    // Given
    MOCKER(vsnprintf_s).stubs().will(returnValue(-1));
    // When
    string ret = Format(100, "Test %d", 42);
    // Then
    EXPECT_EQ(ret, "");
}
