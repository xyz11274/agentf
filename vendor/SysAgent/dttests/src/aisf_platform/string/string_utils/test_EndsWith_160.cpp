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

class TestEndsWith : public ::testing::Test {
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

TEST_F(TestEndsWith, ShouldReturnTrue_WhenStringEndsWithTailWithoutIgnoreCase) {
    // Given
    // 无mock需求
    
    // When
    string s = "hello world";
    string tail = "world";
    bool ignoreCase = false;
    bool ret = EndsWith(s, tail, ignoreCase);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestEndsWith, ShouldReturnFalse_WhenStringDoesNotEndWithTail) {
    // Given
    // 无mock需求
    
    // When
    string s = "hello world";
    string tail = "hello";
    bool ignoreCase = false;
    bool ret = EndsWith(s, tail, ignoreCase);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestEndsWith, ShouldReturnFalse_WhenTailIsEmptyString) {
    // Given
    // 无mock需求
    
    // When
    string s = "hello world";
    string tail = "";
    bool ignoreCase = false;
    bool ret = EndsWith(s, tail, ignoreCase);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestEndsWith, ShouldReturnTrue_WhenStringEndsWithTailWithIgnoreCase) {
    // Given
    // 无mock需求
    
    // When
    string s = "Hello World";
    string tail = "world";
    bool ignoreCase = true;
    bool ret = EndsWith(s, tail, ignoreCase);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestEndsWith, ShouldReturnFalse_WhenStringEndsWithPartialMatchOfTailWithIgnoreCase) {
    // Given
    // 无mock需求
    
    // When
    string s = "Hello Worl";
    string tail = "world";
    bool ignoreCase = true;
    bool ret = EndsWith(s, tail, ignoreCase);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestEndsWith, ShouldReturnTrue_WhenBothStringsAreEmpty) {
    // Given
    // 无mock需求
    
    // When
    string s = "";
    string tail = "";
    bool ignoreCase = false;
    bool ret = EndsWith(s, tail, ignoreCase);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestEndsWith, ShouldReturnFalse_WhenStringIsShorterThanTail) {
    // Given
    // 无mock需求
    
    // When
    string s = "hi";
    string tail = "hello";
    bool ignoreCase = false;
    bool ret = EndsWith(s, tail, ignoreCase);
    
    // Then
    EXPECT_EQ(ret, false);
}

