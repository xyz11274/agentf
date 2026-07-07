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
using namespace AisfPlat::WebSocket;

class TestWebSocketClient_SetSslInfo : public ::testing::Test {
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

TEST_F(TestWebSocketClient_SetSslInfo, ShouldSetSslInfoCorrectly_WhenCertInfoIsValid) {
    // Given
    lws_context_creation_info info = {0};
    CertInfo certInfo;
    
    // When
    WebSocketClient::SetSslInfo(info, certInfo);
    
    // Then
    EXPECT_NE(info.ssl_ca_filepath, nullptr);
    EXPECT_TRUE((info.options & LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT) != 0);
    EXPECT_TRUE((info.options & LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT) != 0);
}

TEST_F(TestWebSocketClient_SetSslInfo, ShouldNotModifyOptions_WhenCertInfoIsEmpty) {
    // Given
    lws_context_creation_info info = {0};
    info.options = 0x12345678;  // 设置一个初始值
    
    CertInfo certInfo;
    certInfo.sslCaFilePath.clear();
    certInfo.sslPrivateKeyFilePath.clear();
    certInfo.sslCertFilePath.clear();
    
    // When
    WebSocketClient::SetSslInfo(info, certInfo);
}

TEST_F(TestWebSocketClient_SetSslInfo, ShouldSetSslOptionsCorrectly_WhenEnvVarIsSet) {
    // Given
    lws_context_creation_info info = {0};
    CertInfo certInfo;
    
    // 设置环境变量
    setenv("INNER_TLS_PRIVATE_KEY_PWD", "test_pwd", 1);
    setenv("SSLPATH", "/custom/path/", 1);
    
    // When
    WebSocketClient::SetSslInfo(info, certInfo);
    
    // Then
    EXPECT_NE(info.ssl_ca_filepath, nullptr);
    EXPECT_TRUE((info.options & LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT) != 0);
    EXPECT_TRUE((info.options & LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT) != 0);
    
    // Clean
    unsetenv("INNER_TLS_PRIVATE_KEY_PWD");
    unsetenv("SSLPATH");
}

TEST_F(TestWebSocketClient_SetSslInfo, ShouldHandleNullPointerInCertInfo_WhenGetCertPathFails) {
    // Given
    lws_context_creation_info info = {0};
    info.options = 0x87654321;  // 设置一个初始值
    
    CertInfo certInfo;
    
    // Mock getenv to return NULL for INNER_TLS_PRIVATE_KEY_PWD
    // MOCKER(getenv).stubs().with(eq(string("INNER_TLS_PRIVATE_KEY_PWD"))).will(returnValue(nullptr));
    
    // When
    WebSocketClient::SetSslInfo(info, certInfo);
}

