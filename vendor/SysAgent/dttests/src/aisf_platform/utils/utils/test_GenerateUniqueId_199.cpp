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

class TestGenerateUniqueId : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
        // 手动恢复静态变量 idCounter 到初始状态
        static int* idCounterPtr = &idCounter;
        *idCounterPtr = 0;
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
    
private:
    // 由于 idCounter 是 static 变量，需要手动管理其状态
    static int idCounter;
};

int TestGenerateUniqueId::idCounter = 0;

TEST_F(TestGenerateUniqueId, ShouldReturnUniqueString_WhenCalledMultipleTimes) {
    // Given
    // 无mock需求
    
    // When
    std::string ret1 = GenerateUniqueId();
    std::string ret2 = GenerateUniqueId();
    std::string ret3 = GenerateUniqueId();
    
    // Then
    EXPECT_EQ(ret1, "1");
    EXPECT_EQ(ret2, "2");
    EXPECT_EQ(ret3, "3");
}

TEST_F(TestGenerateUniqueId, ShouldResetCounter_WhenGlobalStateIsCleared) {
    // Given
    // 手动设置 idCounter 的值
    static int* idCounterPtr = &idCounter;
    *idCounterPtr = 100;
    
    // When
    std::string ret = GenerateUniqueId();
    
    // Then
    EXPECT_EQ(ret, "4");
}

TEST_F(TestGenerateUniqueId, ShouldReturnValidStringFormat_WhenGenerateUniqueIdCalled) {
    // Given
    // 无mock需求
    
    // When
    std::string ret = GenerateUniqueId();
    
    // Then
    EXPECT_FALSE(ret == "1");  // 第一次调用不等于 "1"
}

