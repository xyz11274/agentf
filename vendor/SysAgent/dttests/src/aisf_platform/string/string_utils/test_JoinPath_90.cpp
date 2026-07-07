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

class TestJoinPath : public ::testing::Test {
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

TEST_F(TestJoinPath, ShouldReturnJoinedPathWithForwardSlashes_WhenBaseAndChildContainBackwardSlashes) {
    // Given
    // No mocks needed
    
    // When
    string base = "C:\\Users\\Admin";
    string child = "Documents\\file.txt";
    string result = JoinPath(base, child);
    
    // Then
    EXPECT_EQ(result, "C:/Users/Admin/Documents/file.txt");
}

TEST_F(TestJoinPath, ShouldRemoveTrailingSlashFromBase_WhenBaseEndsWithSlash) {
    // Given
    // No mocks needed
    
    // When
    string base = "/home/user/";
    string child = "config.ini";
    string result = JoinPath(base, child);
    
    // Then
    EXPECT_EQ(result, "/home/user/config.ini");
}

TEST_F(TestJoinPath, ShouldConcatenatePathsDirectly_WhenChildStartsWithSlash) {
    // Given
    // No mocks needed
    
    // When
    string base = "home/user";
    string child = "/documents/file.txt";
    string result = JoinPath(base, child);
    
    // Then
    EXPECT_EQ(result, "home/user/documents/file.txt");
}

TEST_F(TestJoinPath, ShouldJoinPathsCorrectly_WhenBothBaseAndChildHaveNoSpecialCharacters) {
    // Given
    // No mocks needed
    
    // When
    string base = "folder1";
    string child = "subfolder/file.txt";
    string result = JoinPath(base, child);
    
    // Then
    EXPECT_EQ(result, "folder1/subfolder/file.txt");
}

TEST_F(TestJoinPath, ShouldHandleEmptyStringInBase_WhenBaseIsEmptyButChildIsNot) {
    // Given
    // No mocks needed
    
    // When
    string base = "";
    string child = "path/to/file";
    string result = JoinPath(base, child);
    
    // Then
    EXPECT_EQ(result, "/path/to/file");
}

TEST_F(TestJoinPath, ShouldReturnChildPathOnly_WhenBaseIsEmptyAndChildStartsWithSlash) {
    // Given
    // No mocks needed
    
    // When
    string base = "";
    string child = "/absolute/path";
    string result = JoinPath(base, child);
    
    // Then
    EXPECT_EQ(result, "/absolute/path");
}

