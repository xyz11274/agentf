/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringBuffer_Get : public ::testing::Test {
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

TEST_F(TestStringBuffer_Get, ShouldReturnFalse_WhenBufferIsEmpty) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    SafeString data("test_func", __LINE__);

    // When
    bool ret = buffer.Get(data);

    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStringBuffer_Get, ShouldReturnFalse_WhenSafeStringResetFail) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    buffer.Add("hello", 5);
    SafeString data("test_func", __LINE__);

    MOCKER(&SafeString::Reset).stubs().will(returnValue(false));

    // When
    bool ret = buffer.Get(data);

    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStringBuffer_Get, ShouldReturnFalse_WhenSafeStringWriteFail) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    buffer.Add("hello", 5);
    SafeString data("test_func", __LINE__);

    MOCKER(&SafeString::Write).stubs().will(returnValue(false));

    // When
    bool ret = buffer.Get(data);

    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStringBuffer_Get, ShouldReturnTrue_WhenBufferHasDataAndOperationSucceeds) {
    // Given
    StringBuffer buffer("test_buffer", 1024);
    buffer.Add("hello", 5);
    SafeString data("test_func", __LINE__);

    // When
    bool ret = buffer.Get(data);

    // Then
    EXPECT_EQ(ret, true);
}

