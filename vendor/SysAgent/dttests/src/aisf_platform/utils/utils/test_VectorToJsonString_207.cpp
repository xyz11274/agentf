/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include <iomanip>
#include <thread>
#include <filesystem>
#include <unistd.h>
#include <sys/sysinfo.h>
#include "framework_common/utils/net_util.h"
#include "framework_common/utils/time_util.h"
#include "aisf_platform/utils/utils.h"
#include "aisf_platform/common_define.h"

using namespace mockcpp;
using namespace AisfPlat::Utils;

class TestVectorToJsonString : public ::testing::Test {
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

TEST_F(TestVectorToJsonString, ShouldReturnEmptyJsonArray_WhenInputVectorIsEmpty) {
    // Given
    std::vector<std::string> vec;
    // When
    std::string result = VectorToJsonString(vec);
    // Then
    EXPECT_EQ(result, "[]");
}

TEST_F(TestVectorToJsonString, ShouldReturnValidJsonArray_WhenInputVectorHasOneElement) {
    // Given
    std::vector<std::string> vec = {"hello"};
    // When
    std::string result = VectorToJsonString(vec);
    // Then
    EXPECT_EQ(result, "[\n\t\"hello\"\n]");
}

TEST_F(TestVectorToJsonString, ShouldReturnValidJsonArray_WhenInputVectorHasMultipleElements) {
    // Given
    std::vector<std::string> vec = {"a", "b", "c"};
    // When
    std::string result = VectorToJsonString(vec);
    // Then
    EXPECT_EQ(result, "[\n\t\"a\",\n\t\"b\",\n\t\"c\"\n]");
}

TEST_F(TestVectorToJsonString, ShouldHandleSpecialCharactersInStrings_WhenInputVectorContainsSpecialChars) {
    // Given
    std::vector<std::string> vec = {"hello \"world\"", "test\\backslash"};
    // When
    std::string result = VectorToJsonString(vec);
    // Then
    EXPECT_EQ(result, "[\n\t\"hello \\\"world\\\"\",\n\t\"test\\\\backslash\"\n]");
}

TEST_F(TestVectorToJsonString, ShouldReturnJsonArrayWithEscapedNewline_WhenInputVectorContainsNewline) {
    // Given
    std::vector<std::string> vec = {"line1\nline2"};
    // When
    std::string result = VectorToJsonString(vec);
    // Then
    EXPECT_EQ(result, "[\n\t\"line1\\nline2\"\n]");
}

