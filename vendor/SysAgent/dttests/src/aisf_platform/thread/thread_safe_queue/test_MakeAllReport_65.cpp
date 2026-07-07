/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "framework_common/utils/file_util.h"
#include "aisf_platform/thread/thread_safe_queue.h"
#include <cstdint>

using namespace mockcpp;
using namespace AisfPlat;

class TestThreadSafeQueueBase_MakeAllReport : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
        // Clean up files created during tests
        unlink("/tmp/writeThreadSafeQueue");
        unlink("/tmp/ThreadSafeQueue.txt");
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

// Mock instance with custom Report behavior
class MockThreadSafeQueue : public ThreadSafeQueueBase {
public:
    MockThreadSafeQueue(const string& name, bool enableMetric) : ThreadSafeQueueBase(name, enableMetric) {}
    void ChildReport(stringstream& out) override {
        out << "Mock Report Content\n";
    }
};

// TEST_F(TestThreadSafeQueueBase_MakeAllReport, ShouldRemoveFileAndWriteReport_WhenFileExistsAndInstancesValid) {
//     // Given
//     // MOCKER(&CMFrm::Utils::CheckFileExist).stubs().with(eq("/tmp/writeThreadSafeQueue")).will(returnValue(true));
//     MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
//     MOCKER(&remove).stubs().with(eq("/tmp/writeThreadSafeQueue")).will(returnValue(0));
    
//     auto instance = std::make_unique<MockThreadSafeQueue>("test_pool", true);
//     ThreadSafeQueueBase::instances.second.insert(instance.get());

//     // When
//     ThreadSafeQueueBase::MakeAllReport();

//     // // Then
//     // // 验证文件是否被删除
//     // EXPECT_EQ(access("/tmp/writeThreadSafeQueue", F_OK), -1);  // 文件不存在
    
//     // 验证报告文件是否被写入
//     FILE* fp = fopen("/tmp/ThreadSafeQueue.txt", "r");
//     EXPECT_NE(fp, nullptr);
//     if (fp) {
//         fclose(fp);
//     }

//     // Clean
//     ThreadSafeQueueBase::instances.second.clear();
// }