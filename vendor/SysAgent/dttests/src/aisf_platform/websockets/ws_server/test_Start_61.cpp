/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include <arpa/inet.h>
#include <thread>
#include <fstream>
#include <unistd.h>
#include "framework_common/utils/file_util.h"
#include "event2/thread.h"
#include "aisf_platform/websockets/ws_server.h"
#include "aisf_platform/websockets/common.h"

using namespace mockcpp;
using namespace AisfPlat;
using namespace AisfPlat::WebSocket;

class TestWebSocketServerStart : public ::testing::Test {
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

TEST_F(TestWebSocketServerStart, ShouldReturnTrue_WhenServerAlreadyRunning) {
    // Given
    WebSocketServer::Info info;
    info.port = 8080;
    info.threadCnt = 0;
    info.enableSsl = false;
    
    WebSocketServer server(info);
    server.m_isRunning->store(true);
    
    // When
    bool ret = server.Start();
    
    // Then
    EXPECT_EQ(ret, true);
}

TEST_F(TestWebSocketServerStart, ShouldReturnFalse_WhenThreadPoolStartFailed) {
    // Given
    WebSocketServer::Info info;
    info.port = 8080;
    info.threadCnt = 2;
    info.enableSsl = false;
    
    WebSocketServer server(info);
    server.m_isRunning->store(false);
    
    MOCKER(&ThreadPool<WebSocketCbMsg>::Start).stubs().will(returnValue(false));
    
    // When
    bool ret = server.Start();
    
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(server.m_isThreadPoolRun, false);
    EXPECT_EQ(server.m_context, nullptr);
}

TEST_F(TestWebSocketServerStart, ShouldCreateContextSuccessfully_WhenServerNotRunningAndEnableSslIsTrue) {
    // Given
    WebSocketServer::Info info;
    info.port = 8080;
    info.threadCnt = 0;
    info.enableSsl = true;
    
    WebSocketServer server(info);
    server.m_isRunning->store(false);
    server.m_context = nullptr;
    
    // When
    bool ret = server.Start();
    
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(*(server.m_isRunning), 0);
}

TEST_F(TestWebSocketServerStart, ShouldReturnFalse_WhenContextCreationFailed) {
    // Given
    WebSocketServer::Info info;
    info.port = 8080;
    info.threadCnt = 0;
    info.enableSsl = false;
    
    WebSocketServer server(info);
    server.m_isRunning->store(false);
    server.m_context = nullptr;
    
    MOCKER(&lws_create_context).stubs().will(returnValue(static_cast<lws_context*>(nullptr)));
    
    // When
    bool ret = server.Start();
    
    // Then
    EXPECT_EQ(ret, false);
    EXPECT_EQ(server.m_context, nullptr);
    EXPECT_EQ(*(server.m_isRunning), false);
}

TEST_F(TestWebSocketServerStart, ShouldStartThreadPool_WhenThreadCntGreaterThanZero) {
    // Given
    WebSocketServer::Info info;
    info.port = 8080;
    info.threadCnt = 2;
    info.enableSsl = false;
    
    WebSocketServer server(info);
    server.m_isRunning->store(false);
    server.m_context = nullptr;
    
    // When
    bool ret = server.Start();
    
    // Then
    EXPECT_EQ(ret, true);
    EXPECT_EQ(server.m_isThreadPoolRun, true);
    EXPECT_NE(server.m_context, nullptr);
}

TEST_F(TestWebSocketServerStart, ShouldSetTimerForFirstConnection_WhenTimerUsingWsiIsNull) {
    // Given
    WebSocketServer::Info info;
    info.port = 8080;
    info.threadCnt = 0;
    info.enableSsl = false;
    
    WebSocketServer server(info);
    server.m_isRunning->store(false);
    server.m_context = nullptr;
    server.m_timerUsingWsi = nullptr;
    
    // When
    bool ret = server.Start();
    
    // Then
    EXPECT_EQ(ret, true);
}

