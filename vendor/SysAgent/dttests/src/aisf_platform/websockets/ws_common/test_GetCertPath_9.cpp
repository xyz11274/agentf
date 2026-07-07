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

class TestGetCertPath : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
        // 清理环境变量
        unsetenv("INNER_TLS_PRIVATE_KEY_PWD");
        unsetenv("SSLPATH");
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestGetCertPath, ShouldSetCertPathsSuccessfully_WhenEnvVarsAreValid) {
    // Given
    setenv("INNER_TLS_PRIVATE_KEY_PWD", "test_password", 1);
    setenv("SSLPATH", "/custom/path/", 1);
    CertInfo info;

    // When
    GetCertPath(info);

    // Then
    EXPECT_STREQ(info.pwd.Data(), "test_password");
    EXPECT_EQ(info.sslCaFilePath, "/custom/path/ca.crt");
    EXPECT_EQ(info.sslPrivateKeyFilePath, "/custom/path/tls.key.pwd");
    EXPECT_EQ(info.sslCertFilePath, "/custom/path/tls.crt");
}

TEST_F(TestGetCertPath, ShouldUseDefaultSslPath_WhenSSLPATHNotSet) {
    // Given
    setenv("INNER_TLS_PRIVATE_KEY_PWD", "some_key", 1);
    unsetenv("SSLPATH");  // 确保未设置
    CertInfo info;

    // When
    GetCertPath(info);

    // Then
    EXPECT_STREQ(info.pwd.Data(), "some_key");
    EXPECT_EQ(info.sslCaFilePath, "/opt/sharevolume/ssl/certca.crt");
    EXPECT_EQ(info.sslPrivateKeyFilePath, "/opt/sharevolume/ssl/certtls.key.pwd");
    EXPECT_EQ(info.sslCertFilePath, "/opt/sharevolume/ssl/certtls.crt");
}

TEST_F(TestGetCertPath, ShouldReturnEarly_WhenINNER_TLS_PRIVATE_KEY_PWDNotSet) {
    // Given
    unsetenv("INNER_TLS_PRIVATE_KEY_PWD");
    setenv("SSLPATH", "/valid/path/", 1);
    CertInfo info;

    // When
    GetCertPath(info);

    // Then
    EXPECT_STREQ(info.pwd.Data(), "");
    EXPECT_EQ(info.sslCaFilePath, "");
    EXPECT_EQ(info.sslPrivateKeyFilePath, "");
    EXPECT_EQ(info.sslCertFilePath, "");
}

TEST_F(TestGetCertPath, ShouldHandleEmptyPwdValue_WhenINNER_TLS_PRIVATE_KEY_PWDIsEmpty) {
    // Given
    setenv("INNER_TLS_PRIVATE_KEY_PWD", "", 1);
    setenv("SSLPATH", "/another/path/", 1);
    CertInfo info;

    // When
    GetCertPath(info);

    // Then
    EXPECT_STREQ(info.pwd.Data(), "");
    EXPECT_EQ(info.sslCaFilePath, "/another/path/ca.crt");
    EXPECT_EQ(info.sslPrivateKeyFilePath, "/another/path/tls.key.pwd");
    EXPECT_EQ(info.sslCertFilePath, "/another/path/tls.crt");
}

