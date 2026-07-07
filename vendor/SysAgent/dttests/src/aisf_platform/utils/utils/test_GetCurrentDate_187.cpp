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

class TestGetCurrentDate : public ::testing::Test {
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

TEST_F(TestGetCurrentDate, ShouldReturnCurrentDateInYYYYMMDDFormat_WhenCalled) {
    // Given
    // 由于time、localtime、strftime都是系统函数，且在当前上下文中没有明确指出需要mock，
    // 所以不进行mock操作
    
    // When
    std::string result = GetCurrentDate();

    // Then
    // 校验返回值是否符合YYYYMMDD格式
    EXPECT_EQ(result.length(), 8);
    // 格式为YYYYMMDD，所以前4位是年份，接下来2位是月份，最后2位是日期
    // 这里我们只做基本格式校验，不深入验证日期有效性
    EXPECT_TRUE(isdigit(result[0]) && isdigit(result[1]) && isdigit(result[2]) && isdigit(result[3]));
    EXPECT_TRUE(isdigit(result[4]) && isdigit(result[5]));
    EXPECT_TRUE(isdigit(result[6]) && isdigit(result[7]));

    // Clean
    // 无特殊清理工作
}

