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

class TestLower : public ::testing::Test {
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

TEST_F(TestLower, ShouldReturnEmptyString_WhenInputIsEmpty) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string s = "";
    string result = Lower(s);
    
    // Then
    EXPECT_TRUE(result.empty());
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestLower, ShouldReturnAllLowerCase_WhenInputHasMixedCase) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string s = "AbCdEfGhIjKlMnOpQrStUvWxYz";
    string result = Lower(s);
    
    // Then
    EXPECT_EQ(result, "abcdefghijklmnopqrstuvwxyz");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestLower, ShouldReturnSameString_WhenInputIsAlreadyLowerCase) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string s = "abcdefghijklmnopqrstuvwxyz";
    string result = Lower(s);
    
    // Then
    EXPECT_EQ(result, s);
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestLower, ShouldHandleSpecialCharacters_WhenInputContainsNonAlphabeticChars) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string s = "ABC123!@#def";
    string result = Lower(s);
    
    // Then
    EXPECT_EQ(result, "abc123!@#def");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

TEST_F(TestLower, ShouldReturnCorrectlyWithUnicode_WhenInputContainsUnicodeChars) {
    // Given
    // 无mock
    // 无复杂类型构造
    // 无全局/成员变量修改
    
    // When
    string s = "Hello世界123";
    string result = Lower(s);
    
    // Then
    EXPECT_EQ(result, "hello世界123");
    
    // Clean
    // 无mock清理
    // 无内存清理
    // 无状态恢复
}

