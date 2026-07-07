/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include <iomanip>
#include <thread>
#include <filesystem>
#include <unistd.h>
#include <sys/sysinfo.h>
#include "framework_common/utils/net_util.h"
#include "framework_common/utils/time_util.h"
#include "aisf_platform/utils/utils.h"
#include "aisf_platform/common_define.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestJoinIpPort : public ::testing::Test {
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

TEST_F(TestJoinIpPort, ShouldReturnIpWithPort_WhenIpDoesNotContainColon) {
    // Given
    // 无mock需求
    
    // When
    string ip = "192.168.1.1";
    string port = "8080";
    string result = AisfPlat::Utils::JoinIpPort(ip, port);
    
    // Then
    EXPECT_EQ(result, "192.168.1.1:8080");
}

TEST_F(TestJoinIpPort, ShouldAddBracketsAroundIpv6_WhenIpContainsColonButNoBrackets) {
    // Given
    // 无mock需求
    
    // When
    string ip = "2001:db8::1";
    string port = "8080";
    string result = AisfPlat::Utils::JoinIpPort(ip, port);
    
    // Then
    EXPECT_EQ(result, "[2001:db8::1]:8080");
}

TEST_F(TestJoinIpPort, ShouldReturnIpWithPort_WhenIpv6AlreadyHasBrackets) {
    // Given
    // 无mock需求
    
    // When
    string ip = "[2001:db8::1]";
    string port = "8080";
    string result = AisfPlat::Utils::JoinIpPort(ip, port);
    
    // Then
    EXPECT_EQ(result, "[2001:db8::1]:8080");
}

TEST_F(TestJoinIpPort, ShouldHandleEmptyStringForIpAndPort) {
    // Given
    // 无mock需求
    
    // When
    string ip = "";
    string port = "";
    string result = AisfPlat::Utils::JoinIpPort(ip, port);
    
    // Then
    EXPECT_EQ(result, ":");
}

TEST_F(TestJoinIpPort, ShouldHandleSpecialCharactersInPort) {
    // Given
    // 无mock需求
    
    // When
    string ip = "localhost";
    string port = "8080/tcp";
    string result = AisfPlat::Utils::JoinIpPort(ip, port);
    
    // Then
    EXPECT_EQ(result, "localhost:8080/tcp");
}

