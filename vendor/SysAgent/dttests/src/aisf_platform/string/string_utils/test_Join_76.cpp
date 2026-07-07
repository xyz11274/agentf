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

class TestJoin : public ::testing::Test {
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

TEST_F(TestJoin, ShouldReturnEmptyString_WhenInputElemsIsEmpty) {
    // Given
    vector<string> elems;
    string delimiter = "";
    // When
    string ret = Join(elems, delimiter);
    // Then
    EXPECT_EQ(ret, "");
}

TEST_F(TestJoin, ShouldReturnSingleElement_WhenInputElemsHasOneElement) {
    // Given
    vector<string> elems = {"hello"};
    string delimiter = "";
    // When
    string ret = Join(elems, delimiter);
    // Then
    EXPECT_EQ(ret, "hello");
}

TEST_F(TestJoin, ShouldReturnJoinedStringWithoutTrailingDelimiter_WhenMultipleElementsExist) {
    // Given
    vector<string> elems = {"a", "b", "c"};
    string delimiter = ", ";
    // When
    string ret = Join(elems, delimiter);
    // Then
    EXPECT_EQ(ret, "a, b, c");
}

TEST_F(TestJoin, ShouldReturnJoinedStringWithCustomDelimiter_WhenDelimiterIsSpecified) {
    // Given
    vector<string> elems = {"apple", "banana", "cherry"};
    string delimiter = " | ";
    // When
    string ret = Join(elems, delimiter);
    // Then
    EXPECT_EQ(ret, "apple | banana | cherry");
}

TEST_F(TestJoin, ShouldHandleSpecialCharactersInDelimiterCorrectly_WhenDelimiterContainsSpecialChars) {
    // Given
    vector<string> elems = {"test1", "test2"};
    string delimiter = "\n";
    // When
    string ret = Join(elems, delimiter);
    // Then
    EXPECT_EQ(ret, "test1\ntest2");
}

