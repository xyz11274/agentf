/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/websockets/safe_lws_string.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestSafeLwsStringData : public ::testing::Test {
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

TEST_F(TestSafeLwsStringData, ShouldReturnNullPtr_WhenSafeLwsStringIsEmpty) {
    // Given
    SafeLwsString safeLwsString;
    
    // When
    unsigned char* result = safeLwsString.Data();
    
    // Then
    EXPECT_EQ(result, nullptr);
}

TEST_F(TestSafeLwsStringData, ShouldReturnValidPtr_WhenSafeLwsStringHasData) {
    // Given
    SafeLwsString safeLwsString(10);
    const char* testData = "test data";
    safeLwsString.Write(testData, strlen(testData));
    
    // When
    unsigned char* result = safeLwsString.Data();
    
    // Then
    EXPECT_NE(result, nullptr);
}

TEST_F(TestSafeLwsStringData, ShouldReturnCorrectOffsetPtr_WhenSafeLwsStringHasData) {
    // Given
    SafeLwsString safeLwsString(10);
    const char* testData = "test data";
    safeLwsString.Write(testData, strlen(testData));
    
    // When
    unsigned char* result = safeLwsString.Data();
    unsigned char* expected = (unsigned char*)(safeLwsString.m_data.Get() + SafeLwsString::PRE_LEN);
    
    // Then
    EXPECT_EQ(result, expected);
}

