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

class TestWebSocketClientConnect : public ::testing::Test {
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

TEST_F(TestWebSocketClientConnect, ShouldReturnTrue_WhenFirstServerConnectSucceed) {
    // Given
    MOCKER(lws_client_connect_via_info).stubs()
        .will(returnValue((lws*)0x1))
        .then(ignoreReturnValue());
    
    WebSocketClient::Info info;
    info.protocol = "test_protocol";
    info.enableSsl = true;
    WebSocketClient client(info);
    
    vector<WebSocketServerInfo> addrs;
    WebSocketServerInfo addr1, addr2;
    addr1.ip = "192.168.1.1";
    addr1.port = 8080;
    addr1.path = "/path1";
    addr2.ip = "192.168.1.2";
    addr2.port = 8081;
    addr2.path = "/path2";
    addrs.push_back(addr1);
    addrs.push_back(addr2);
    
    // When
    bool ret = client.Connect(addrs);
    
    // Then
    EXPECT_TRUE(ret);
    EXPECT_EQ(client.GetServerInfo().ip, addr1.ip);
    EXPECT_EQ(client.GetServerInfo().port, addr1.port);
    EXPECT_EQ(client.GetServerInfo().path, addr1.path);
}

TEST_F(TestWebSocketClientConnect, ShouldReturnFalse_WhenAllServersConnectFailed) {
    // Given
    MOCKER(lws_client_connect_via_info).stubs()
        .will(returnValue(static_cast<lws*>(nullptr)))
        .then(returnValue(static_cast<lws*>(nullptr)));
    
    WebSocketClient::Info info;
    info.protocol = "test_protocol";
    info.enableSsl = true;
    WebSocketClient client(info);
    
    vector<WebSocketServerInfo> addrs;
    WebSocketServerInfo addr1, addr2;
    addr1.ip = "192.168.1.1";
    addr1.port = 8080;
    addr1.path = "/path1";
    addr2.ip = "192.168.1.2";
    addr2.port = 8081;
    addr2.path = "/path2";
    addrs.push_back(addr1);
    addrs.push_back(addr2);
    
    // When
    bool ret = client.Connect(addrs);
    
    // Then
    EXPECT_FALSE(ret);
    EXPECT_EQ(client.GetServerInfo().ip, "");
    EXPECT_EQ(client.GetServerInfo().port, 0);
    EXPECT_EQ(client.GetServerInfo().path, "");
}

TEST_F(TestWebSocketClientConnect, ShouldReturnTrue_WhenSecondServerConnectSucceed) {
    // Given
    MOCKER(lws_client_connect_via_info).stubs()
        .will(returnValue(static_cast<lws*>(nullptr)))
        .then(returnValue((lws*)0x2));
    
    WebSocketClient::Info info;
    info.protocol = "test_protocol";
    info.enableSsl = true;
    WebSocketClient client(info);
    
    vector<WebSocketServerInfo> addrs;
    WebSocketServerInfo addr1, addr2;
    addr1.ip = "192.168.1.1";
    addr1.port = 8080;
    addr1.path = "/path1";
    addr2.ip = "192.168.1.2";
    addr2.port = 8081;
    addr2.path = "/path2";
    addrs.push_back(addr1);
    addrs.push_back(addr2);
    
    // When
    bool ret = client.Connect(addrs);
    
    // Then
    EXPECT_TRUE(ret);
    EXPECT_EQ(client.GetServerInfo().ip, addr2.ip);
    EXPECT_EQ(client.GetServerInfo().port, addr2.port);
    EXPECT_EQ(client.GetServerInfo().path, addr2.path);
}

TEST_F(TestWebSocketClientConnect, ShouldReturnFalse_WhenEmptyAddrsVectorPassed) {
    // Given
    WebSocketClient::Info info;
    info.protocol = "test_protocol";
    info.enableSsl = true;
    WebSocketClient client(info);
    
    vector<WebSocketServerInfo> addrs;
    
    // When
    bool ret = client.Connect(addrs);
    
    // Then
    EXPECT_FALSE(ret);
    EXPECT_EQ(client.GetServerInfo().ip, "");
    EXPECT_EQ(client.GetServerInfo().port, 0);
    EXPECT_EQ(client.GetServerInfo().path, "");
}

