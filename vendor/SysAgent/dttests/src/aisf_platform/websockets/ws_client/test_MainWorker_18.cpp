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
using namespace AisfPlat::Utils;

class TestWebSocketClientMainWorker : public ::testing::Test {
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

TEST_F(TestWebSocketClientMainWorker, ShouldExitLoop_WhenIsRunningIsFalse) {
    // Given
    WebSocketClient::Info info;
    info.name = "test_client";
    
    WebSocketClient client(info);
    
    // 设置m_isRunning为false
    auto isRunning = client.m_isRunning;
    isRunning->store(false);
    
    // Mock lws_service 和 lws_context_destroy
    MOCKER(lws_service).stubs().will(ignoreReturnValue());
    MOCKER(lws_context_destroy).stubs().will(ignoreReturnValue());
    
    // Mock RecordStatus
    MOCKER(AisfPlat::WebSocket::RecordStatus).expects(exactly(2));
    
    // When
    client.MainWorker();
    
    // Then
    // 由于isRunning为false，lws_service不会被调用，但lws_context_destroy会被调用
    // RecordStatus会被调用两次
}

// TEST_F(TestWebSocketClientMainWorker, ShouldSetThreadName_WhenThreadStarts) {
//     // Given
//     WebSocketClient::Info info;
//     info.name = "test_client";
    
//     WebSocketClient client(info);
    
//     // 设置m_isRunning为true
//     auto isRunning = client.m_isRunning;
//     isRunning->store(true);
    
//     // Mock lws_service 和 lws_context_destroy
//     MOCKER(lws_service).stubs().will(ignoreReturnValue());
//     MOCKER(lws_context_destroy).stubs().will(ignoreReturnValue());
    
//     // Mock SetThreadName
//     // MOCKER(AisfPlat::Utils::SetThreadName).with(eq("test_client_ws"));
//     MOCKER(AisfPlat::Utils::SetThreadName).stubs().will(ignoreReturnValue());

//     // Mock RecordStatus
//     MOCKER(AisfPlat::WebSocket::RecordStatus).expects(exactly(2));
    
//     // When
//     client.MainWorker();
    
//     // Then
//     // 验证SetThreadName被调用
// }

// TEST_F(TestWebSocketClientMainWorker, ShouldCallLwsServiceMultipleTimes_WhenIsRunningIsTrue) {
//     // Given
//     WebSocketClient::Info info;
//     info.name = "test_client";
    
//     WebSocketClient client(info);
    
//     // 设置m_isRunning为true
//     auto isRunning = client.m_isRunning;
//     isRunning->store(true);
    
//     // Mock lws_service 和 lws_context_destroy
//     // MOCKER(lws_service).with(any(), eq(MILLI_IN_SECOND));
//     MOCKER(lws_service).stubs().with(any(), eq(MILLI_IN_SECOND)).will(ignoreReturnValue());
//     MOCKER(lws_context_destroy).stubs().will(ignoreReturnValue());
    
//     // Mock RecordStatus
//     MOCKER(AisfPlat::WebSocket::RecordStatus).expects(exactly(2));
    
//     // When
//     client.MainWorker();
    
//     // Then
//     // 验证lws_service被多次调用
// }

TEST_F(TestWebSocketClientMainWorker, ShouldNotCallLwsService_WhenIsRunningIsFalseFromStart) {
    // Given
    WebSocketClient::Info info;
    info.name = "test_client";
    
    WebSocketClient client(info);
    
    // 设置m_isRunning为false
    auto isRunning = client.m_isRunning;
    isRunning->store(false);
    
    // Mock lws_service 和 lws_context_destroy
    MOCKER(lws_service).expects(never());
    MOCKER(lws_context_destroy).stubs().will(ignoreReturnValue());
    
    // Mock RecordStatus
    MOCKER(AisfPlat::WebSocket::RecordStatus).expects(exactly(2));
    
    // When
    client.MainWorker();
    
    // Then
    // 验证lws_service未被调用
}
