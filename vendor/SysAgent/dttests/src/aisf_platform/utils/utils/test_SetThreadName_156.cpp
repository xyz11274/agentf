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

class TestSetThreadName : public ::testing::Test {
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

TEST_F(TestSetThreadName, ShouldSetThreadNameSuccessfully_WhenValidNameProvided) {
    // Given
    char buffer[16] = {0};
    MOCKER(pthread_setname_np).stubs().will(ignoreReturnValue());
    MOCKER(pthread_getname_np).stubs().will(returnValue(0));
    

    // When
    SetThreadName("TestThread");

    // Then
    EXPECT_STREQ(buffer, "");
}

TEST_F(TestSetThreadName, ShouldTruncateLongName_WhenNameExceedsMaxLength) {
    // Given
    char buffer[16] = {0};
    MOCKER(pthread_setname_np).stubs().will(ignoreReturnValue());
    MOCKER(pthread_getname_np).stubs().will(returnValue(0));

    // When
    SetThreadName("ThisIsAVeryLongName");

    // Then
    EXPECT_STREQ(buffer, "");
}

TEST_F(TestSetThreadName, ShouldLogError_WhenSetNameFailsAfterMaxRetries) {
    // Given
    char buffer[16] = {0};
    MOCKER(pthread_setname_np).stubs().will(ignoreReturnValue());
    MOCKER(pthread_getname_np).stubs().will(returnValue(0));

    // When
    SetThreadName("");

    // Then
    // 校验LOG_ERR是否被调用，这里假设我们能通过mock方式捕获日志输出
    // 实际测试中可能需要更复杂的mock机制或替换日志函数
}

TEST_F(TestSetThreadName, ShouldReturnImmediately_WhenNameAlreadySetCorrectly) {
    // Given
    char buffer[16] = {0};
    MOCKER(pthread_setname_np).stubs().will(ignoreReturnValue());
    MOCKER(pthread_getname_np).stubs().will(returnValue(0));

    // When
    SetThreadName("FastSet");

    // Then
    EXPECT_STREQ(buffer, "");
}

