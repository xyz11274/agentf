/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/websockets/safe_lws_string.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestSafeLwsStringInitial : public ::testing::Test {
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

TEST_F(TestSafeLwsStringInitial, ShouldReturnFalse_WhenLengthIsZero) {
    // Given
    SafeLwsString lwsStr;
    // When
    bool ret = lwsStr.Initial(0);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestSafeLwsStringInitial, ShouldReturnFalse_WhenResetFails) {
    // Given
    SafeLwsString lwsStr;
    MOCKER(&SafeString::Reset).stubs().will(returnValue(false));
    // When
    bool ret = lwsStr.Initial(10);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestSafeLwsStringInitial, ShouldReturnTrue_WhenLengthIsValidAndResetSucceeds) {
    // Given
    SafeLwsString lwsStr;
    // When
    bool ret = lwsStr.Initial(10);
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestSafeLwsStringInitial, ShouldWriteMagicCorrectly_WhenInitialSucceeds) {
    // Given
    SafeLwsString lwsStr;
    // When
    bool ret = lwsStr.Initial(5);
    // Then
    EXPECT_EQ(ret, true);
    unsigned long long* magicPtr = reinterpret_cast<unsigned long long*>(lwsStr.Data());
    EXPECT_NE(*magicPtr, 0xC0FFEE00DEADBEEF);
}

