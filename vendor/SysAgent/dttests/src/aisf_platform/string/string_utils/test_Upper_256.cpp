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

class TestUpper : public ::testing::Test {
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

TEST_F(TestUpper, ShouldReturnEmptyString_WhenInputIsEmpty) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string input = "";
    string ret = Upper(input);
    
    // Then
    EXPECT_EQ(ret, "");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestUpper, ShouldReturnUppercaseString_WhenInputHasLowercaseLetters) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string input = "hello world";
    string ret = Upper(input);
    
    // Then
    EXPECT_EQ(ret, "HELLO WORLD");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestUpper, ShouldReturnSameString_WhenInputHasNoAlphabeticCharacters) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string input = "123!@#";
    string ret = Upper(input);
    
    // Then
    EXPECT_EQ(ret, "123!@#");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestUpper, ShouldReturnMixedCaseString_WhenInputContainsBothCases) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string input = "Hello World 123";
    string ret = Upper(input);
    
    // Then
    EXPECT_EQ(ret, "HELLO WORLD 123");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestUpper, ShouldHandleUnicodeCharacters_WhenInputContainsNonAsciiChars) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string input = "你好World";
    string ret = Upper(input);
    
    // Then
    EXPECT_EQ(ret, "你好WORLD");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

