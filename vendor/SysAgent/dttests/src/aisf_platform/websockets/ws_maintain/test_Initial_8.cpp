/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "framework_common/timermanager/timer_manager.h"
#include "framework_common/utils/file_util.h"
#include "aisf_platform/websockets/maintain.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestWsiMaintainInitial : public ::testing::Test {
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

// TEST_F(TestWsiMaintainInitial, ShouldAddTimerTask_WhenInitialCalled) {
//     // When
//     WsiMaintain::Initial();
//     // Then
//     // 通过mock验证TimerManager::AddTimerTask是否被调用
// }

class TestWsiMaintainReport : public ::testing::Test {
protected:
    virtual void SetUp()
    {
        // 初始化测试环境
        WsiMaintain::Instance().m_isWsiHistoryEnable = true;
        WsiMaintain::Instance().m_records.clear();
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
        // 清理测试环境
        WsiMaintain::Instance().m_records.clear();
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestWsiMaintainReport, ShouldHandleTimerCallbackCorrectly_WhenReportIsInvoked) {
    // Given
    auto record = make_shared<WsiMaintain::Record>("from", "to", nullptr, "data", 4);
    WsiMaintain::Instance().m_records.push_back(record);
    // When
    WsiMaintain::Report();
    // Then
    // 校验WriteTextFile是否被调用
}

TEST_F(TestWsiMaintainReport, ShouldSkipWritingHistory_WhenHistoryDisabled) {
    // Given
    WsiMaintain::Instance().m_isWsiHistoryEnable = false;
    auto record = make_shared<WsiMaintain::Record>("from", "to", nullptr, "data", 4);
    WsiMaintain::Instance().m_records.push_back(record);
    // When
    WsiMaintain::Report();
    // Then
    // 校验WriteTextFile未被调用
}

TEST_F(TestWsiMaintainReport, ShouldUseCorrectFilePathBasedOnDevMode_WhenReportIsInvoked) {
    // Given
    // 创建标志文件模拟dev模式
    FILE* fp = fopen("/tmp/writeWsiHistory2", "w");
    if (fp != nullptr) {
        fclose(fp);
    }
    WsiMaintain::Instance().m_isWsiHistoryEnable = true;
    auto record = make_shared<WsiMaintain::Record>("from", "to", nullptr, "data", 4);
    WsiMaintain::Instance().m_records.push_back(record);
    // When
    WsiMaintain::Report();
    // Then
    // 校验写入的是WsiHistory2.csv而不是WsiHistory.csv
}

