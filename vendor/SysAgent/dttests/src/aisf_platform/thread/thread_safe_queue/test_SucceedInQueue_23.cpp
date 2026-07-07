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

class TestThreadSafeQueueBase_SucceedInQueue : public ::testing::Test {
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

TEST_F(TestThreadSafeQueueBase_SucceedInQueue, ShouldUpdateMetrics_WhenEnableMetricIsTrue) {
    // Given
    // ThreadSafeQueueBase queue("test_queue", true);
    // ThreadSafeQueueMsgBaseMock msg;
    // EXPECT_CALL(msg, Size()).WillRepeatedly(Return(100));
    MockThreadSafeQueueMsg item(3600000000000LL, 100);  // 1小时时间戳，大小为100

    ThreadSafeQueueBase* queueBase = new ThreadSafeQueue<TestMessage> ("test_queue", 100, true);

    // When
    queueBase->SucceedInQueue(item);

    // Then
    EXPECT_EQ(queueBase->metrics[0].inQueueCount, 1);
    EXPECT_EQ(queueBase->metrics[0].inQueueSize, 100);
    EXPECT_EQ(queueBase->metrics[0].succeedCount, 1);
    EXPECT_EQ(queueBase->metrics[0].succeedSize, 100);

    // Clean
    delete queueBase;    
}

// TEST_F(TestThreadSafeQueueBase_SucceedInQueue, ShouldDoNothing_WhenEnableMetricIsFalse) {
//     // Given
//     ThreadSafeQueueBase queue("test_queue", false);
//     ThreadSafeQueueMsgBaseMock msg;
//     EXPECT_CALL(msg, Size()).WillRepeatedly(Return(200));

//     // When
//     queue.SucceedInQueue(msg);

//     // Then
//     EXPECT_EQ(queue.metrics[0].inQueueCount, 0);
//     EXPECT_EQ(queue.metrics[0].inQueueSize, 0);
//     EXPECT_EQ(queue.metrics[0].succeedCount, 0);
//     EXPECT_EQ(queue.metrics[0].succeedSize, 0);
// }

// TEST_F(TestThreadSafeQueueBase_SucceedInQueue, ShouldUpdateCorrectMinuteSlot_WhenItemHasDifferentMinutePosition) {
//     // Given
//     ThreadSafeQueueBase queue("test_queue", true);
    
//     // Mock two messages with different minute positions
//     ThreadSafeQueueMsgBaseMock msg1;
//     ThreadSafeQueueMsgBaseMock msg2;
    
//     // Set up the first message to have minute position 0
//     EXPECT_CALL(msg1, Size()).WillRepeatedly(Return(150));
//     EXPECT_CALL(msg1, inQueTimeMinPosInHour).WillRepeatedly(Return(0));
    
//     // Set up the second message to have minute position 5
//     EXPECT_CALL(msg2, Size()).WillRepeatedly(Return(150));
//     EXPECT_CALL(msg2, inQueTimeMinPosInHour).WillRepeatedly(Return(5));

//     // When
//     queue.SucceedInQueue(msg1);
//     queue.SucceedInQueue(msg2);

//     // Then
//     EXPECT_EQ(queue.metrics[0].inQueueCount, 1);
//     EXPECT_EQ(queue.metrics[0].inQueueSize, 150);
//     EXPECT_EQ(queue.metrics[0].succeedCount, 1);
//     EXPECT_EQ(queue.metrics[0].succeedSize, 150);

//     EXPECT_EQ(queue.metrics[5].inQueueCount, 1);
//     EXPECT_EQ(queue.metrics[5].inQueueSize, 150);
//     EXPECT_EQ(queue.metrics[5].succeedCount, 1);
//     EXPECT_EQ(queue.metrics[5].succeedSize, 150);
// }

