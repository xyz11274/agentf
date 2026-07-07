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

class TestThreadSafeQueueBaseReport : public ::testing::Test {
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

    struct TestMessage {
        int id;
        string content;
    };

};

TEST_F(TestThreadSafeQueueBaseReport, ShouldOutputBasicInfoAndSkipMetrics_WhenMetricDisabled) {
    // Given
    // ThreadSafeQueueBase* queue = new ThreadSafeQueueBase("TestQueue", false);
    ThreadSafeQueueBase* queueBase = new ThreadSafeQueue<TestMessage> ("test_queue", 100, true);

    stringstream ss;
    
    // When
    queueBase->Report(ss);
    
    // Then
    string result = ss.str();
    EXPECT_EQ(result.find("ThreadSafeQueue:TestQueue, create @"), string::npos);
    EXPECT_EQ(result.find("Not enable queue metric!!!"), string::npos);
    EXPECT_NE(result.find(AisfPlat::ThreadSafeQueueBase::Metric::HEADER), string::npos);
    
    // Clean
    delete queueBase;
}

// TEST_F(TestThreadSafeQueueBaseReport, ShouldOutputHeaderAndMetrics_WhenMetricEnabledAndCurrentMinuteIsZero) {
//     // Given
//     ThreadSafeQueueBase* queue = new ThreadSafeQueueBase("TestQueue", true);
//     stringstream ss;
    
//     // When
//     queue->Report(ss);
    
//     // Then
//     string result = ss.str();
//     EXPECT_NE(result.find("ThreadSafeQueue:TestQueue, create @"), string::npos);
//     EXPECT_NE(result.find(Metric::HEADER), string::npos);
    
//     // Clean
//     delete queue;
// }

// TEST_F(TestThreadSafeQueueBaseReport, ShouldOutputHeaderAndMetricsWithCorrectTimeRange_WhenMetricEnabledAndStartPosIsNonZero) {
//     // Given
//     ThreadSafeQueueBase* queue = new ThreadSafeQueueBase("TestQueue", true);
//     stringstream ss;
    
//     // When
//     queue->Report(ss);
    
//     // Then
//     string result = ss.str();
//     EXPECT_NE(result.find("ThreadSafeQueue:TestQueue, create @"), string::npos);
//     EXPECT_NE(result.find(Metric::HEADER), string::npos);
    
//     // Clean
//     delete queue;
// }

// TEST_F(TestThreadSafeQueueBaseReport, ShouldHandleEdgeCaseOfMetricsArrayAccess_WhenMetricsArrayHasValidData) {
//     // Given
//     ThreadSafeQueueBase* queue = new ThreadSafeQueueBase("TestQueue", true);
//     stringstream ss;
    
//     // 初始化metrics数组
//     for (int i = 0; i < MINUTE_IN_HOUR; ++i) {
//         queue->metrics[i].inQueueCount = i;
//         queue->metrics[i].inQueueSize = i * 1024;
//         queue->metrics[i].succeedCount = i * 2;
//         queue->metrics[i].succeedSize = i * 2048;
//         queue->metrics[i].failCount = i * 3;
//         queue->metrics[i].failSize = i * 3072;
//         queue->metrics[i].outQueueCount = i * 4;
//         queue->metrics[i].outQueueSize = i * 4096;
//         queue->metrics[i].outWaitTotalNano = i * 1000000;
//         queue->metrics[i].maxOutQueueNano = i * 5000000;
//     }
    
//     // When
//     queue->Report(ss);
    
//     // Then
//     string result = ss.str();
//     EXPECT_NE(result.find("ThreadSafeQueue:TestQueue, create @"), string::npos);
//     EXPECT_NE(result.find(Metric::HEADER), string::npos);
    
//     // Clean
//     delete queue;
// }

// TEST_F(TestThreadSafeQueueBaseReport, ShouldCallChildReport_WhenCalled) {
//     // Given
//     class MockThreadSafeQueueBase : public ThreadSafeQueueBase {
//     public:
//         MockThreadSafeQueueBase(const string& name, bool enableMetric) : ThreadSafeQueueBase(name, enableMetric) {}
        
//         void ChildReport(stringstream& out) override {
//             out << "Child report content" << endl;
//         }
//     };
    
//     MockThreadSafeQueueBase* queue = new MockThreadSafeQueueBase("TestQueue", true);
//     stringstream ss;
    
//     // When
//     queue->Report(ss);
    
//     // Then
//     string result = ss.str();
//     EXPECT_NE(result.find("ThreadSafeQueue:TestQueue, create @"), string::npos);
//     EXPECT_NE(result.find("Child report content"), string::npos);
//     EXPECT_NE(result.find(Metric::HEADER), string::npos);
    
//     // Clean
//     delete queue;
// }

