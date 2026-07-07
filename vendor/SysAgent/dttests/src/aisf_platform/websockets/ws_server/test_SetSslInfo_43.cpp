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

class TestWebSocketServer_SetSslInfo : public ::testing::Test {
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

TEST_F(TestWebSocketServer_SetSslInfo, ShouldSetSslInfoSuccessfully_WhenAllInputsAreValid) {
    // Given
    lws_context_creation_info info = {0};
    CertInfo certInfo;
    certInfo.pwd.SetToSafe();
    const char* envPwd = "test_password";
    const char* envSslPath = "/custom/ssl/path/";
    
    setenv("INNER_TLS_PRIVATE_KEY_PWD", envPwd, 1);
    setenv("SSLPATH", envSslPath, 1);
    
    // When
    WebSocketServer::SetSslInfo(info, certInfo);
    
    // Then
    EXPECT_EQ(info.ka_time, 60);
    EXPECT_EQ(info.ka_interval, 15);
    EXPECT_EQ(info.ka_probes, 5);
    EXPECT_STREQ(info.ssl_private_key_password, envPwd);
    EXPECT_STREQ(info.ssl_cert_filepath, "/custom/ssl/path/tls.crt");
    EXPECT_STREQ(info.ssl_private_key_filepath, "/custom/ssl/path/tls.key.pwd");
    EXPECT_STREQ(info.ssl_ca_filepath, "/custom/ssl/path/ca.crt");
    
    // Clean
    unsetenv("INNER_TLS_PRIVATE_KEY_PWD");
    unsetenv("SSLPATH");
}

TEST_F(TestWebSocketServer_SetSslInfo, ShouldHandleNullPointerInCertInfo_WhenPwdIsNull) {
    // Given
    lws_context_creation_info info = {0};
    CertInfo certInfo;
    certInfo.pwd.SetToSafe();
    certInfo.pwd.Reset(0); // Make pwd empty
    
    const char* envPwd = nullptr;
    const char* envSslPath = "/custom/ssl/path/";
    
    setenv("SSLPATH", envSslPath, 1);
    unsetenv("INNER_TLS_PRIVATE_KEY_PWD"); // Ensure pwd is null
    
    // When
    WebSocketServer::SetSslInfo(info, certInfo);
    
    // Then
    EXPECT_NE(info.options & LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT, 0);
    EXPECT_NE(info.client_tls_1_3_plus_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.tls1_3_plus_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.client_ssl_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.ssl_cipher_list, WSI_CIPER_LIST);
    EXPECT_EQ(info.ka_time, 60);
    EXPECT_EQ(info.ka_interval, 15);
    EXPECT_EQ(info.ka_probes, 5);
    EXPECT_NE(info.ssl_private_key_password, nullptr);
    EXPECT_STREQ(info.ssl_cert_filepath, "");
    EXPECT_STREQ(info.ssl_private_key_filepath, "");
    EXPECT_STREQ(info.ssl_ca_filepath, "");
    
    // Clean
    unsetenv("SSLPATH");
}

TEST_F(TestWebSocketServer_SetSslInfo, ShouldUseDefaultCertPath_WhenEnvVarNotSet) {
    // Given
    lws_context_creation_info info = {0};
    CertInfo certInfo;
    certInfo.pwd.SetToSafe();
    
    const char* envPwd = "default_password";
    unsetenv("SSLPATH"); // Remove SSLPATH
    
    setenv("INNER_TLS_PRIVATE_KEY_PWD", envPwd, 1);
    
    // When
    WebSocketServer::SetSslInfo(info, certInfo);
    
    // Then
    EXPECT_NE(info.options & LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT, 0);
    EXPECT_NE(info.client_tls_1_3_plus_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.tls1_3_plus_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.client_ssl_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.ssl_cipher_list, WSI_CIPER_LIST);
    EXPECT_EQ(info.ka_time, 60);
    EXPECT_EQ(info.ka_interval, 15);
    EXPECT_EQ(info.ka_probes, 5);
    EXPECT_STREQ(info.ssl_private_key_password, envPwd);
    EXPECT_STREQ(info.ssl_cert_filepath, "/opt/sharevolume/ssl/certtls.crt");
    EXPECT_STREQ(info.ssl_private_key_filepath, "/opt/sharevolume/ssl/certtls.key.pwd");
    EXPECT_STREQ(info.ssl_ca_filepath, "/opt/sharevolume/ssl/certca.crt");
    
    // Clean
    unsetenv("INNER_TLS_PRIVATE_KEY_PWD");
}

TEST_F(TestWebSocketServer_SetSslInfo, ShouldFailGracefully_WhenEnvVarInnerTlsPwdNotSet) {
    // Given
    lws_context_creation_info info = {0};
    CertInfo certInfo;
    certInfo.pwd.SetToSafe();
    
    const char* envSslPath = "/custom/ssl/path/";
    setenv("SSLPATH", envSslPath, 1);
    unsetenv("INNER_TLS_PRIVATE_KEY_PWD"); // Not set
    
    // When
    WebSocketServer::SetSslInfo(info, certInfo);
    
    // Then
    EXPECT_NE(info.options & LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT, 0);
    EXPECT_NE(info.client_tls_1_3_plus_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.tls1_3_plus_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.client_ssl_cipher_list, WSI_CIPER_LIST);
    EXPECT_NE(info.ssl_cipher_list, WSI_CIPER_LIST);
    EXPECT_EQ(info.ka_time, 60);
    EXPECT_EQ(info.ka_interval, 15);
    EXPECT_EQ(info.ka_probes, 5);
    EXPECT_NE(info.ssl_private_key_password, nullptr);
    EXPECT_STREQ(info.ssl_cert_filepath, "");
    EXPECT_STREQ(info.ssl_private_key_filepath, "");
    EXPECT_STREQ(info.ssl_ca_filepath, "");
    
    // Clean
    unsetenv("SSLPATH");
}

