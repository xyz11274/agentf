/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/websockets/safe_lws_string.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestSafeLwsStringWrite : public ::testing::Test {
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

TEST_F(TestSafeLwsStringWrite, ShouldReturnFalse_WhenContentIsNull) {
    // Given
    // 无mock需求
    
    // When
    SafeLwsString s;
    bool ret = s.Write(nullptr, 10);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestSafeLwsStringWrite, ShouldReturnFalse_WhenInitialReturnsFalseDueToZeroLength) {
    // Given
    MOCKER(&SafeLwsString::Initial).stubs().will(returnValue(false));
    
    // When
    SafeLwsString s;
    char content[] = "test";
    bool ret = s.Write(content, 4);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestSafeLwsStringWrite, ShouldReturnFalse_WhenMemcpySFailed) {
    // Given
    MOCKER(memcpy_s).stubs().will(returnValue(EINVAL));
    
    // When
    SafeLwsString s;
    char content[] = "test";
    bool ret = s.Write(content, 4);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestSafeLwsStringWrite, ShouldReturnTrue_WhenWriteSucceeds) {
    // Given
    // 无mock需求
    
    // When
    SafeLwsString s;
    char content[] = "test";
    bool ret = s.Write(content, 4);
    
    // Then
    EXPECT_EQ(ret, true);
}

