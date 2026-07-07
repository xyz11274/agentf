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

class TestCountSubChar : public ::testing::Test {
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

TEST_F(TestCountSubChar, ShouldReturnZero_WhenStringIsEmptyAndIgnoreCaseIsFalse) {
    // Given
    // No mocks needed
    
    // When
    string s = "";
    char c = 'a';
    bool ignoreCase = false;
    int result = CountSubChar(s, c, ignoreCase);
    
    // Then
    EXPECT_EQ(result, 0);
}

TEST_F(TestCountSubChar, ShouldReturnOne_WhenCharacterExistsOnceInStringAndIgnoreCaseIsFalse) {
    // Given
    // No mocks needed
    
    // When
    string s = "hello";
    char c = 'e';
    bool ignoreCase = false;
    int result = CountSubChar(s, c, ignoreCase);
    
    // Then
    EXPECT_EQ(result, 1);
}

TEST_F(TestCountSubChar, ShouldReturnTwo_WhenCharacterExistsTwiceInStringAndIgnoreCaseIsFalse) {
    // Given
    // No mocks needed
    
    // When
    string s = "aabbcc";
    char c = 'b';
    bool ignoreCase = false;
    int result = CountSubChar(s, c, ignoreCase);
    
    // Then
    EXPECT_EQ(result, 2);
}

TEST_F(TestCountSubChar, ShouldReturnThree_WhenCharacterExistsThreeTimesInStringAndIgnoreCaseIsTrue) {
    // Given
    // No mocks needed
    
    // When
    string s = "AaBbCcA";
    char c = 'a';
    bool ignoreCase = true;
    int result = CountSubChar(s, c, ignoreCase);
    
    // Then
    EXPECT_EQ(result, 3);
}

TEST_F(TestCountSubChar, ShouldReturnZero_WhenCharacterDoesNotExistInStringAndIgnoreCaseIsTrue) {
    // Given
    // No mocks needed
    
    // When
    string s = "hello world";
    char c = 'z';
    bool ignoreCase = true;
    int result = CountSubChar(s, c, ignoreCase);
    
    // Then
    EXPECT_EQ(result, 0);
}

TEST_F(TestCountSubChar, ShouldReturnFive_WhenCharacterExistsMultipleTimesWithIgnoreCaseFlagSet) {
    // Given
    // No mocks needed
    
    // When
    string s = "AbCdEfGhIjKlMnOpQrStUvWxYzA";
    char c = 'a';
    bool ignoreCase = true;
    int result = CountSubChar(s, c, ignoreCase);
    
    // Then
    EXPECT_EQ(result, 2);
}

