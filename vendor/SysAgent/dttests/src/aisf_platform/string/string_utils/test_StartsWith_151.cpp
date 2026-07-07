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

class TestStartsWith : public ::testing::Test {
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

TEST_F(TestStartsWith, ShouldReturnTrue_WhenStringStartsWithHeadWithoutIgnoreCase) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "hello world";
    string head = "hello";
    bool ret = StartsWith(s, head, false);
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStartsWith, ShouldReturnFalse_WhenStringDoesNotStartWithHeadWithoutIgnoreCase) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "hello world";
    string head = "world";
    bool ret = StartsWith(s, head, false);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStartsWith, ShouldReturnTrue_WhenStringStartsWithHeadWithIgnoreCase) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "Hello World";
    string head = "hello";
    bool ret = StartsWith(s, head, true);
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStartsWith, ShouldReturnFalse_WhenStringDoesNotStartWithHeadWithIgnoreCase) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "Hello World";
    string head = "world";
    bool ret = StartsWith(s, head, true);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStartsWith, ShouldReturnTrue_WhenHeadIsSubstringAtBeginningOfSWithIgnoreCase) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "HELLO world";
    string head = "hello";
    bool ret = StartsWith(s, head, true);
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStartsWith, ShouldReturnTrue_WhenHeadIsEmptyButSIsNotEmpty) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "hello";
    string head = "";
    bool ret = StartsWith(s, head, false);
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStartsWith, ShouldReturnTrue_WhenBothStringsAreEmpty) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "";
    string head = "";
    bool ret = StartsWith(s, head, false);
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestStartsWith, ShouldReturnFalse_WhenSIsShorterThanHead) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "hi";
    string head = "hello";
    bool ret = StartsWith(s, head, false);
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestStartsWith, ShouldReturnTrue_WhenSEqualsHeadWithIgnoreCase) {
    // Given
    // 无需要mock的外部函数
    // 无复杂的结构体或对象构造
    // 无全局变量/类成员变量需要初始化
    // When
    string s = "Test";
    string head = "test";
    bool ret = StartsWith(s, head, true);
    // Then
    EXPECT_EQ(ret, true);
}

