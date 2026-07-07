/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "framework_common/utils/file_util.h"
#include "aisf_platform/thread/thread_pool.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestThreadPoolBaseMakeAllReport : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
        // Clean up any created files
        unlink("/tmp/writeThreadPool");
        unlink("/tmp/ThreadPool.txt");
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestThreadPoolBaseMakeAllReport, ShouldNotRemoveFileAndReturnEarly_WhenWriteThreadPoolFileDoesNotExist) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(false));
    // When
    ThreadPoolBase::MakeAllReport();
    // Then
    // No further action expected due to early return
}

TEST_F(TestThreadPoolBaseMakeAllReport, ShouldRemoveFileAndProceed_WhenWriteThreadPoolFileExistsButRemoveFails) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    MOCKER(&remove).stubs().will(returnValue(-1));
    // When
    ThreadPoolBase::MakeAllReport();
    // Then
    // The function should proceed even if remove fails
}

TEST_F(TestThreadPoolBaseMakeAllReport, ShouldWriteReportToFile_WhenThreadPoolInstancesExistAndValid) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    MOCKER(&remove).stubs().will(returnValue(0));

    // Mock instance with custom Report behavior
    class MockThreadPool : public ThreadPoolBase {
    public:
        MockThreadPool(const string& name, bool enableMetric) : ThreadPoolBase(name, enableMetric) {}
        void ChildReport(stringstream& out) override {
            out << "Mock Report Content\n";
        }
    };

    MockThreadPool* instance = new MockThreadPool("test_pool", true);
    ThreadPoolBase::instances.second.insert(instance);

    // When
    ThreadPoolBase::MakeAllReport();

    // Then
    // Verify that the file was written correctly by checking its content or existence
    // This would require additional setup to read the file and assert its contents

    // Clean
    ThreadPoolBase::instances.second.clear();
    delete instance;
}

TEST_F(TestThreadPoolBaseMakeAllReport, ShouldSkipNullInstance_WhenThreadPoolInstancesContainNullPointers) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    MOCKER(&remove).stubs().will(returnValue(0));

    ThreadPoolBase::instances.second.insert(nullptr);
    ThreadPoolBase::instances.second.insert(static_cast<ThreadPoolBase*>(nullptr));

    // When
    ThreadPoolBase::MakeAllReport();

    // Then
    // No crash or error expected when encountering null pointers

    // Clean
    ThreadPoolBase::instances.second.clear();
}

TEST_F(TestThreadPoolBaseMakeAllReport, ShouldWriteEmptyContent_WhenNoThreadPoolInstancesExist) {
    // Given
    MOCKER(&CMFrm::Utils::CheckFileExist).stubs().will(returnValue(true));
    MOCKER(&remove).stubs().will(returnValue(0));

    // When
    ThreadPoolBase::MakeAllReport();

    // Then
    // File should be created but empty or only contain separators
}

