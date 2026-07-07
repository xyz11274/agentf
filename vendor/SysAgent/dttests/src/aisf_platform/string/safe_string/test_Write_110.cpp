/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestSafeStringWrite : public ::testing::Test {
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

TEST_F(TestSafeStringWrite, ShouldReturnFalse_WhenInputIsNullptrOrZeroLength) {
    // Given
    SafeString s("test", 1, 10);
    // When
    bool ret1 = s.Write(nullptr, 0);
    bool ret2 = s.Write("test", 0);
    bool ret3 = s.Write(nullptr, 5);
    // Then
    EXPECT_EQ(ret1, false);
    EXPECT_EQ(ret2, false);
    EXPECT_EQ(ret3, false);
}

TEST_F(TestSafeStringWrite, ShouldReturnFalse_WhenLengthExceedsAvailableSize) {
    // Given
    SafeString s("test", 1, 10);
    // When
    bool ret = s.Write("very_long_string_data", 20);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestSafeStringWrite, ShouldReturnTrue_WhenMemcpySucceeds) {
    // Given
    SafeString s("test", 1, 20);
    MOCKER(memcpy_s).stubs().will(returnValue(EOK));
    // When
    bool ret = s.Write("hello", 5);
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestSafeStringWrite, ShouldReturnFalse_WhenMemcpySFailed) {
    // Given
    SafeString s("test", 1, 20);
    MOCKER(memcpy_s).stubs().will(returnValue(EINVAL));
    // When
    bool ret = s.Write("world", 5);
    // Then
    EXPECT_EQ(ret, false);
}

