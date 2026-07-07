/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include <thread>
#include "event2/thread.h"
#include "framework_common/utils/file_util.h"
#include "aisf_platform/websockets/ws_client.h"
#include "aisf_platform/websockets/common.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestWebSocketClientStart : public ::testing::Test {
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

TEST_F(TestWebSocketClientStart, ShouldReturnTrue_WhenClientAlreadyRunning) {
    // Given
    WebSocketClient::Info info;
    info.name = "test_client";
    info.protocol = "test_protocol";
    info.enableSsl = false;
    info.threadCnt = 0;
    
    WebSocketClient client(info);
    client.m_isRunning->store(true);
    
    // When
    bool ret = client.Start();
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestWebSocketClientStart, ShouldReturnFalse_WhenThreadPoolStartFails) {
    // Given
    WebSocketClient::Info info;
    info.name = "test_client";
    info.protocol = "test_protocol";
    info.enableSsl = false;
    info.threadCnt = 1;
    
    WebSocketClient client(info);
    client.m_isRunning->store(false);
    
    MOCKER(&ThreadPool<WebSocketCbMsg>::Start).stubs().will(returnValue(false));
    
    // When
    bool ret = client.Start();
    
    // Then
    EXPECT_EQ(ret, false);
}

TEST_F(TestWebSocketClientStart, ShouldInitContextAndReturnTrue_WhenContextIsNullAndSslDisabled) {
    // Given
    WebSocketClient::Info info;
    info.name = "test_client";
    info.protocol = "test_protocol";
    info.enableSsl = false;
    info.threadCnt = 0;
    
    WebSocketClient client(info);
    client.m_isRunning->store(false);
    client.m_context = nullptr;
    
    // When
    bool ret = client.Start();
    
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_NE(client.m_context, nullptr);
    EXPECT_EQ(*(client.m_isRunning), true);
}

TEST_F(TestWebSocketClientStart, ShouldReturnFalse_WhenLwsCreateContextFails) {
    // Given
    WebSocketClient::Info info;
    info.name = "test_client";
    info.protocol = "test_protocol";
    info.enableSsl = true;
    info.threadCnt = 0;
    
    WebSocketClient client(info);
    client.m_isRunning->store(false);
    client.m_context = nullptr;
    
    MOCKER(&lws_create_context).stubs().will(returnValue(static_cast<lws_context*>(nullptr)));
    
    // When
    bool ret = client.Start();
    
    // Then
    EXPECT_EQ(ret, false);
}

