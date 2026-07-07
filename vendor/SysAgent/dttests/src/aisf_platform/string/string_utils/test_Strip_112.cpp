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

class TestStrip : public ::testing::Test {
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

TEST_F(TestStrip, ShouldReturnEmptyString_WhenInputIsEmptyString) {
    // Given
    // No mocks needed
    
    // When
    string s = "";
    string result = Strip(s);
    
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestStrip, ShouldReturnOriginalString_WhenNoLeadingOrTrailingSpaces) {
    // Given
    // No mocks needed
    
    // When
    string s = "hello world";
    string result = Strip(s);
    
    // Then
    EXPECT_EQ(result, "hello world");
}

TEST_F(TestStrip, ShouldRemoveLeadingSpaces_WhenInputHasLeadingSpaces) {
    // Given
    // No mocks needed
    
    // When
    string s = "   hello";
    string result = Strip(s);
    
    // Then
    EXPECT_EQ(result, "hello");
}

TEST_F(TestStrip, ShouldRemoveTrailingSpaces_WhenInputHasTrailingSpaces) {
    // Given
    // No mocks needed
    
    // When
    string s = "hello   ";
    string result = Strip(s);
    
    // Then
    EXPECT_EQ(result, "hello");
}

TEST_F(TestStrip, ShouldRemoveBothLeadingAndTrailingSpaces_WhenInputHasSpacesOnBothSides) {
    // Given
    // No mocks needed
    
    // When
    string s = "   hello   ";
    string result = Strip(s);
    
    // Then
    EXPECT_EQ(result, "hello");
}

TEST_F(TestStrip, ShouldReturnOnlyNonSpaceChars_WhenInputContainsOnlySpaces) {
    // Given
    // No mocks needed
    
    // When
    string s = "     ";
    string result = Strip(s);
    
    // Then
    EXPECT_EQ(result, "");
}

TEST_F(TestStrip, ShouldHandleMixedWhitespaceCharacters_WhenInputHasTabsAndSpaces) {
    // Given
    // No mocks needed
    
    // When
    string s = "\t  hello\t  ";
    string result = Strip(s);
    
    // Then
    EXPECT_EQ(result, "hello");
}

