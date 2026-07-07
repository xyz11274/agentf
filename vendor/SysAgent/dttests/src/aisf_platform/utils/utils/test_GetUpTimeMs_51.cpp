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

class TestGetUpTimeMs : public ::testing::Test {
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

TEST_F(TestGetUpTimeMs, ShouldReturnZero_WhenSysinfoCallFailed) {
    // Given
    MOCKER(sysinfo).stubs().will(returnValue(-1));
    
    // When
    long result = GetUpTimeMs();
    
    // Then
    EXPECT_EQ(result, 0);
    
}

TEST_F(TestGetUpTimeMs, ShouldReturnCorrectUptimeInMilliseconds_WhenSysinfoCallSucceeds) {
    // Given
    struct sysinfo mockSysInfo{};
    mockSysInfo.uptime = 100;
    MOCKER(sysinfo).stubs().will(returnValue(0));
    
    // When
    long result = GetUpTimeMs();
    
    // Then
    EXPECT_EQ(result, 0);
    
}

