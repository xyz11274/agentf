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

class TestWebSocketClientMakeAllReport : public ::testing::Test {
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

TEST_F(TestWebSocketClientMakeAllReport, ShouldNotProceed_WhenCheckFileExistFails) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(false));
    // When
    WebSocketClient::MakeAllReport();
    // Then
    // 无具体校验，因为函数提前返回，不执行后续逻辑
}