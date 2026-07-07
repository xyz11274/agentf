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

class TestWebSocketClient_Stop : public ::testing::Test {
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

TEST_F(TestWebSocketClient_Stop, ShouldStopWebSocketClient_WhenClientIsRunning) {
    // Given
    WebSocketClient::Info info;
    info.name = "TestClient";
    WebSocketClient client(info);
    
    // 设置 m_isRunning 为 true
    client.m_isRunning->store(true);
    
    // Mock RecordStatus 函数
    MOCKER(AisfPlat::WebSocket::RecordStatus).stubs().will(ignoreReturnValue());

    // When
    client.Stop();

    // Then
    EXPECT_FALSE(client.m_isRunning->load());
    // sleep_for 调用无法直接mock，但可以通过验证流程正确性间接确认

    // Clean
    // 无需特别清理，因为是栈对象
}

TEST_F(TestWebSocketClient_Stop, ShouldNotStopWebSocketClient_WhenClientIsNotRunning) {
    // Given
    WebSocketClient::Info info;
    info.name = "TestClient";
    WebSocketClient client(info);
    
    // 设置 m_isRunning 为 false
    client.m_isRunning->store(false);
    
    // Mock RecordStatus 函数，确保它不会被调用
    MOCKER(AisfPlat::WebSocket::RecordStatus).expects(never());

    // When
    client.Stop();

    // Then
    EXPECT_FALSE(client.m_isRunning->load());

    // Clean
    // 无需特别清理，因为是栈对象
}

