/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestSafeString_Swap : public ::testing::Test {
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

TEST_F(TestSafeString_Swap, ShouldSwapMembersSuccessfully_WhenTwoSafeStringObjectsAreExchanged) {
    // Given
    SafeString str1("test", 1, 10, false);
    SafeString str2("test", 2, 15, true);

    // When
    str1.Swap(str2);
}

TEST_F(TestSafeString_Swap, ShouldHandleSelfSwapCorrectly_WhenSwappingWithItself) {
    // Given
    SafeString str("test", 1, 10, true);

    // When
    str.Swap(str);

    // Then
    EXPECT_NE(str.headPos, nullptr);
    EXPECT_NE(str.tailPos, nullptr);
    EXPECT_NE(str.data, nullptr);
    EXPECT_EQ(str.length, 10);
    EXPECT_EQ(str.safeDestroy, true);
}

TEST_F(TestSafeString_Swap, ShouldPreserveSafeDestroyFlagDuringSwap_WhenBothObjectsHaveDifferentSafeStates) {
    // Given
    SafeString str1("test", 1, 10, true);   // safeDestroy = true
    SafeString str2("test", 2, 15, false);  // safeDestroy = false

    // When
    str1.Swap(str2);

    // Then
    EXPECT_EQ(str1.safeDestroy, false);
    EXPECT_EQ(str2.safeDestroy, true);
}

