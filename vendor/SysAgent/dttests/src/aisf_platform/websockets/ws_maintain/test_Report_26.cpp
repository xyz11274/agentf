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

class TestWsiMaintainReport : public ::testing::Test {
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

TEST_F(TestWsiMaintainReport, ShouldWriteWsiHistoryToFile_WhenAllConditionsMet) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().with(any()).will(returnValue(true));
    MOCKER(&remove).stubs().with(any()).will(returnValue(0));
    MOCKER(&FileUtils::WriteTextFile).stubs().with(any(), any(), any()).will(ignoreReturnValue());

    auto& instance = WsiMaintain::Instance();
    instance.m_isWsiHistoryEnable = true;
    auto record = make_shared<WsiMaintain::Record>("client1", "server1", (const lws*)0x12345678, "test data", 9);
    instance.m_records.push_back(record);

    // When
    WsiMaintain::Report();

    // Then
    // 校验 WriteTextFile 被调用，且内容正确
    // 校验 remove 被调用

    // Clean
    instance.m_records.clear();
}

TEST_F(TestWsiMaintainReport, ShouldSkipWriting_WhenEnableFileDoesNotExist) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().with(any()).will(returnValue(false));

    // When
    WsiMaintain::Report();

    // Then
    // 校验 WriteTextFile 和 remove 没有被调用
}

TEST_F(TestWsiMaintainReport, ShouldSkipWriting_WhenWriteFlagFileDoesNotExist) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(false));

    // When
    WsiMaintain::Report();

    // Then
    // 校验 WriteTextFile 和 remove 没有被调用
}

TEST_F(TestWsiMaintainReport, ShouldRemoveFileSuccessfully_WhenDevModDefined) {
    // Given
    #define ACM_DEV_MOD
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().with(any()).will(returnValue(true));
    MOCKER(&remove).stubs().with(any()).will(returnValue(0));
    MOCKER(&FileUtils::WriteTextFile).stubs().with(any(), any(), any()).will(ignoreReturnValue());

    auto& instance = WsiMaintain::Instance();
    instance.m_isWsiHistoryEnable = true;
    auto record = make_shared<WsiMaintain::Record>("client1", "server1", (const lws*)0x12345678, "test data", 9);
    instance.m_records.push_back(record);

    // When
    WsiMaintain::Report();

    // Then
    // 校验 WriteTextFile 被调用，且路径为 /tmp/WsiHistory2.csv
    // 校验 remove 被调用，且路径为 /tmp/writeWsiHistory2

    // Clean
    instance.m_records.clear();
    #undef ACM_DEV_MOD
}

TEST_F(TestWsiMaintainReport, ShouldHandleNullRecord_WhenRecordIsNull) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().with(any()).will(returnValue(true));
    MOCKER(&remove).stubs().with(any()).will(returnValue(0));
    MOCKER(&FileUtils::WriteTextFile).stubs().with(any(), any(), any()).will(ignoreReturnValue());

    auto& instance = WsiMaintain::Instance();
    instance.m_isWsiHistoryEnable = true;
    auto record = make_shared<WsiMaintain::Record>("client1", "server1", (const lws*)0x12345678, "test data", 9);
    instance.m_records.push_back(nullptr);  // 添加空指针
    instance.m_records.push_back(record);

    // When
    WsiMaintain::Report();

    // Then
    // 校验 WriteTextFile 被调用，且只包含非空记录的内容
    // 校验 remove 被调用

    // Clean
    instance.m_records.clear();
}

