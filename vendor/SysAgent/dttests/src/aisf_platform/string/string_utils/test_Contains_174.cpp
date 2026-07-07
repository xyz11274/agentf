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

class TestContains : public ::testing::Test {
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

TEST_F(TestContains, ShouldReturnTrue_WhenSubstringExistsInOriginalString) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "hello world";
    string sub = "world";
    bool ret = Contains(s, sub, false);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestContains, ShouldReturnFalse_WhenSubstringDoesNotExistInOriginalString) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "hello world";
    string sub = "python";
    bool ret = Contains(s, sub, false);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestContains, ShouldReturnTrue_WhenIgnoreCaseIsTrueAndSubstringMatchesCaseInsensitively) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "Hello World";
    string sub = "WORLD";
    bool ret = Contains(s, sub, true);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestContains, ShouldReturnFalse_WhenIgnoreCaseIsTrueButNoMatchCaseInsensitively) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "Hello World";
    string sub = "Python";
    bool ret = Contains(s, sub, true);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestContains, ShouldReturnTrue_WhenSubstringIsEntireString) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "test";
    string sub = "test";
    bool ret = Contains(s, sub, false);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestContains, ShouldReturnTrue_WhenSubstringIsEmptyAndIgnoreCaseIsFalse) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "hello";
    string sub = "";
    bool ret = Contains(s, sub, false);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestContains, ShouldReturnTrue_WhenSubstringIsEmptyAndIgnoreCaseIsTrue) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "hello";
    string sub = "";
    bool ret = Contains(s, sub, true);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestContains, ShouldReturnFalse_WhenOriginalStringIsEmptyAndSubstringIsNotEmpty) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "";
    string sub = "test";
    bool ret = Contains(s, sub, false);
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestContains, ShouldReturnTrue_WhenSubstringIsAtBeginningOfOriginalString) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "hello world";
    string sub = "hello";
    bool ret = Contains(s, sub, false);
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestContains, ShouldReturnTrue_WhenSubstringIsAtEndOfOriginalString) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量或类成员变量需要初始化
    
    // When
    string s = "hello world";
    string sub = "world";
    bool ret = Contains(s, sub, false);
    
    // Then
    EXPECT_EQ(ret, true);
}

