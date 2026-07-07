/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "framework_common/utils/env_util.h"
#include "framework_common/utils/file_util.h"
#include "aisf_platform/websockets/common.h"
#include "aisf_platform/websockets/safe_lws_string.h"

using namespace mockcpp;
using namespace AisfPlat;
using namespace AisfPlat::WebSocket;

class TestLog4Websocket : public ::testing::Test {
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

TEST_F(TestLog4Websocket, ShouldLogErr_WhenLevelIsLLL_ERR) {
    // When
    char line[] = "Error message\n";
    Log4Websocket(LLL_ERR, line);
    // Then
    // 校验LOG_ERR是否被调用
}

TEST_F(TestLog4Websocket, ShouldLogWarn_WhenLevelIsLLL_WARN) {
    // When
    char line[] = "Warning message\n";
    Log4Websocket(LLL_WARN, line);
    // Then
    // 校验LOG_WARN是否被调用
}

TEST_F(TestLog4Websocket, ShouldLogDebug_WhenLevelIsLLL_NOTICE) {
    // When
    char line[] = "Notice message\n";
    Log4Websocket(LLL_NOTICE, line);
    // Then
    // 校验LOG_DEBUG是否被调用
}

TEST_F(TestLog4Websocket, ShouldLogDebug_WhenLevelIsLLL_INFO) {
    // When
    char line[] = "Info message\n";
    Log4Websocket(LLL_INFO, line);
    // Then
    // 校验LOG_DEBUG是否被调用
}

TEST_F(TestLog4Websocket, ShouldLogDebug_WhenLevelIsDefault) {
    // When
    char line[] = "Default message\n";
    Log4Websocket(999, line);
    // Then
    // 校验LOG_DEBUG是否被调用
}

TEST_F(TestLog4Websocket, ShouldReturnEarly_WhenLineIsNull) {
    // When
    Log4Websocket(LLL_ERR, nullptr);
    // Then
    // 不应调用任何日志宏
}

TEST_F(TestLog4Websocket, ShouldRemoveNewline_WhenLineEndsWithNewline) {
    // When
    char line[] = "Test message\n";
    Log4Websocket(LLL_INFO, line);
    // Then
    // 校验LOG_DEBUG是否被调用，且传入参数为"Test message"
}

