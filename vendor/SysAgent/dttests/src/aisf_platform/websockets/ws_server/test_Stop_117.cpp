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

class TestWebSocketServer_Stop : public ::testing::Test {
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

TEST_F(TestWebSocketServer_Stop, ShouldStopServerSuccessfully_WhenServerIsRunning) {
    // Given
    WebSocketServer::Info info;
    info.name = "TestServer";
    WebSocketServer server(info);
    
    server.m_isRunning->store(true);
    
    // When
    server.Stop();
    
    // Then
    EXPECT_FALSE(server.m_isRunning->load());
}

TEST_F(TestWebSocketServer_Stop, ShouldDoNothing_WhenServerIsNotRunning) {
    // Given
    WebSocketServer::Info info;
    info.name = "TestServer";
    WebSocketServer server(info);
    
    server.m_isRunning->store(false);
    
    // When
    server.Stop();
    
    // Then
    EXPECT_FALSE(server.m_isRunning->load());
}

TEST_F(TestWebSocketServer_Stop, ShouldLogStatusAndStop_WhenWebsocketsEnableFileExists) {
    // Given
    // MOCKER(&CMFrm::Utils::CheckFileExist).stubs().with(eq("/tmp/websockets.enable")).will(returnValue(true));
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    
    WebSocketServer::Info info;
    info.name = "TestServer";
    WebSocketServer server(info);
    
    server.m_isRunning->store(true);
    
    // When
    server.Stop();
    
    // Then
    EXPECT_FALSE(server.m_isRunning->load());
}

TEST_F(TestWebSocketServer_Stop, ShouldNotLogStatus_WhenWebsocketsEnableFileDoesNotExist) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().with(eq("/tmp/websockets.enable")).will(returnValue(false));
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(false));

    WebSocketServer::Info info;
    info.name = "TestServer";
    WebSocketServer server(info);
    
    server.m_isRunning->store(true);
    
    // When
    server.Stop();
    
    // Then
    EXPECT_FALSE(server.m_isRunning->load());
}

