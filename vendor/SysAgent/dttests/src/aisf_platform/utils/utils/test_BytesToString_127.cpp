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

class TestBytesToString : public ::testing::Test {
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

TEST_F(TestBytesToString, ShouldReturnBytesString_WhenSizeLessThanByteMaxNum) {
    // Given
    // 无mock需求
    
    // When
    string result = BytesToString(512);
    
    // Then
    EXPECT_EQ(result, "512B");
}

TEST_F(TestBytesToString, ShouldReturnKBString_WhenSizeBetweenByteMaxAndKBMaxNum) {
    // Given
    // 无mock需求
    
    // When
    string result = BytesToString(2048);
    
    // Then
    EXPECT_EQ(result, "2.00KB");
}

TEST_F(TestBytesToString, ShouldReturnMBString_WhenSizeBetweenKBMaxAndMBMaxNum) {
    // Given
    // 无mock需求
    
    // When
    string result = BytesToString(2097152);
    
    // Then
    EXPECT_EQ(result, "2.00MB");
}

TEST_F(TestBytesToString, ShouldReturnGBString_WhenSizeBetweenMBMaxAndGBMaxNum) {
    // Given
    // 无mock需求
    
    // When
    string result = BytesToString(2147483648);
    
    // Then
    EXPECT_EQ(result, "2.00GB");
}

TEST_F(TestBytesToString, ShouldReturnTBString_WhenSizeBetweenGBMaxAndPBMaxNum) {
    // Given
    // 无mock需求
    
    // When
    string result = BytesToString(1099511627776);
    
    // Then
    EXPECT_EQ(result, "1.00TB");
}

TEST_F(TestBytesToString, ShouldReturnPBString_WhenSizeGreaterThanPBMaxNum) {
    // Given
    // 无mock需求
    
    // When
    string result = BytesToString(1125899906842624);
    
    // Then
    EXPECT_EQ(result, "1.00PB");
}

