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

class TestThreadSafeQueueBase_OutQueue : public ::testing::Test {
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

class MockThreadSafeQueueMsg : public ThreadSafeQueueMsgBase {
public:
    MockThreadSafeQueueMsg(int64_t nano, size_t sz) : ThreadSafeQueueMsgBase(), size(sz) {
        const_cast<int64_t&>(inQueTimeNano) = nano;
        const_cast<int64_t&>(inQueTimeMinPosInHour) = (nano / NANO_IN_MINUTE) % MINUTE_IN_HOUR;
    }
    
    [[nodiscard]] size_t Size() const override {
        return size;
    }
    
    size_t size;
};

const static inline int64_t NANO_IN_MINUTE = (int64_t)60 * 1000 * 1000 * 1000;
const static inline int MINUTE_IN_HOUR = 60;

TEST_F(TestThreadSafeQueueBase_OutQueue, ShouldUpdateMetrics_WhenOutQueueCalledWithValidItem) {
    // Given
    // ThreadSafeQueueBase queue("test_queue", true);
    // TestMsg item;
    // item.SetSize(1024);
    // item.SetInQueTimeNano(Utils::GetTimeNano() - 1000000000); // 1秒前

    MockThreadSafeQueueMsg item(3600000000000LL, 100);  // 1小时时间戳，大小为100

    ThreadSafeQueueBase* queueBase = new ThreadSafeQueue<TestMessage> ("test_queue", 100, true);
    
    // When
    queueBase->OutQueue(item);
    
    // Then
    EXPECT_EQ(queueBase->metrics[0].outQueueCount, 1u);
    EXPECT_EQ(queueBase->metrics[0].outQueueSize, 100u);
        
    // Clean
    delete queueBase;
}