/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestSafeString_SetToSafe : public ::testing::Test {
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

TEST_F(TestSafeString_SetToSafe, ShouldSetSafeFlagToTrue_WhenCallSetToSafe) {
    // Given
    SafeString s("test", 1);
    // When
    s.SetToSafe();
    // Then
    EXPECT_EQ(s.Safe(), true);
}

TEST_F(TestSafeString_SetToSafe, ShouldNotChangeOtherMembers_WhenCallSetToSafe) {
    // Given
    SafeString s("test", 1);
    // 记录初始状态
    auto initial_headPos = s.*(&SafeString::headPos);
    auto initial_tailPos = s.*(&SafeString::tailPos);
    auto initial_data = s.*(&SafeString::data);
    auto initial_length = s.*(&SafeString::length);
    // When
    s.SetToSafe();
    // Then
    EXPECT_EQ(s.*(&SafeString::headPos), initial_headPos);
    EXPECT_EQ(s.*(&SafeString::tailPos), initial_tailPos);
    EXPECT_EQ(s.*(&SafeString::data), initial_data);
    EXPECT_EQ(s.*(&SafeString::length), initial_length);
}

TEST_F(TestSafeString_SetToSafe, ShouldReflectInSafeMethod_WhenSetToSafeCalled) {
    // Given
    SafeString s("test", 1);
    // When
    s.SetToSafe();
    // Then
    EXPECT_EQ(s.Safe(), true);
}

