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

class TestTimeDurationToStr : public ::testing::Test {
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

TEST_F(TestTimeDurationToStr, ShouldReturnNanoSeconds_WhenStampLessThanTimeStepNum) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 999;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "999ns");
}

TEST_F(TestTimeDurationToStr, ShouldReturnMicroSeconds_WhenStampBetweenTimeStepNumAndTimeStepNumSquared) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 1500;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "1.500us");
}

TEST_F(TestTimeDurationToStr, ShouldReturnMilliSeconds_WhenStampBetweenTimeStepNumSquaredAndTimeStepNumCubed) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 2500000;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "2.500ms");
}

TEST_F(TestTimeDurationToStr, ShouldReturnSeconds_WhenStampBetweenTimeStepNumCubedAndSecondInMinute) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 75000000;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "75.000ms");
}

TEST_F(TestTimeDurationToStr, ShouldReturnMinutes_WhenStampBetweenSecondInMinuteAndMinuteInHour) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 3660000;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "3.660ms");
}

TEST_F(TestTimeDurationToStr, ShouldReturnHours_WhenStampLessThanSecondInMinute) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 3660000000;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "3.660s");
}

TEST_F(TestTimeDurationToStr, ShouldReturnHours_WhenStampLessThanMinuteInHour) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 366000000000;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "6.006M");
}

TEST_F(TestTimeDurationToStr, ShouldReturnHours_WhenStampGreaterThanMinuteInHour) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 366000000000;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "6.006M");
}

TEST_F(TestTimeDurationToStr, ShouldReturnHours_Default) {
    // Given
    // 无mock需求
    // When
    uint64_t stamp = 36600000000000;
    string result = TimeDurationToStr(stamp);
    // Then
    EXPECT_EQ(result, "10.010H");
}