/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/websockets/safe_lws_string.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestSafeLwsString_Length : public ::testing::Test {
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

TEST_F(TestSafeLwsString_Length, ShouldReturnZero_WhenSafeLwsStringIsEmpty) {
    // Given
    SafeLwsString safeLwsStr;
    // When
    size_t ret = safeLwsStr.Length();
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestSafeLwsString_Length, ShouldReturnCorrectLength_WhenSafeLwsStringHasContent) {
    // Given
    SafeLwsString safeLwsStr;
    const char* testData = "hello world";
    size_t testLen = strlen(testData);
    safeLwsStr.Write(testData, testLen);
    // When
    size_t ret = safeLwsStr.Length();
    // Then
    EXPECT_EQ(ret, testLen);
}

TEST_F(TestSafeLwsString_Length, ShouldReturnZero_WhenSafeLwsStringIsInitializedWithZeroLength) {
    // Given
    SafeLwsString safeLwsStr(0);
    // When
    size_t ret = safeLwsStr.Length();
    // Then
    EXPECT_EQ(ret, 0);
}

TEST_F(TestSafeLwsString_Length, ShouldReturnCorrectLength_WhenSafeLwsStringIsWrittenWithContent) {
    // Given
    SafeLwsString safeLwsStr;
    const char* testData = "test data for length calculation";
    size_t testLen = strlen(testData);
    safeLwsStr.Write(testData, testLen);
    // When
    size_t ret = safeLwsStr.Length();
    // Then
    EXPECT_EQ(ret, testLen);
}

