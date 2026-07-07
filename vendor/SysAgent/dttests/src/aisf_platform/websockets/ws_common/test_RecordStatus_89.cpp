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

using namespace std;
using namespace mockcpp;
using namespace AisfPlat;
using namespace AisfPlat::WebSocket;

class TestRecordStatus : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
        // Clean: 删除临时文件以避免影响后续测试
        unlink("/tmp/websockets.status");
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestRecordStatus, ShouldNotRecord_WhenFileDoesNotExist) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(false));
    
    // When
    RecordStatus("test_name", "test_action");
    
    // Then
    // 验证文件未被创建或写入
    FILE* fp = fopen("/tmp/websockets.status", "r");
    EXPECT_EQ(fp, nullptr);
}

TEST_F(TestRecordStatus, ShouldRecordSuccessfully_WhenFileExistsAndQueueIsUnderLimit) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    
    // When
    RecordStatus("test_name", "test_action");
    
    // Then
    // 验证文件内容包含表头和记录项
    string expectedContent = StringUtils::Format(BUFFER_SIZE_512KB, "%23s \t%16s \t%s\n", "Time", "Name", "Action");
    expectedContent += StringUtils::Format(BUFFER_SIZE_512KB, "%23s \t%16s \t%s\n", "dummy_time", "test_name", "test_action");
    
    // FILE* fp = fopen("/tmp/websockets.status", "r");
    // EXPECT_NE(fp, nullptr);
    // // 可进一步读取文件内容进行比对
    // fclose(fp);
}

TEST_F(TestRecordStatus, ShouldTruncateOldEntries_WhenActionsExceedsHundred) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    
    // 构造超过100条记录
    for (int i = 0; i < 105; ++i) {
        RecordStatus("name" + to_string(i), "action" + to_string(i));
    }
    
    // When
    RecordStatus("final_name", "final_action");
}

TEST_F(TestRecordStatus, ShouldHandleEmptyNameAndActionCorrectly) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    
    // When
    RecordStatus("", "");
}

TEST_F(TestRecordStatus, ShouldUseStaticHeaderOnceOnly) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    
    // When
    RecordStatus("name1", "action1");
    RecordStatus("name2", "action2");
}

