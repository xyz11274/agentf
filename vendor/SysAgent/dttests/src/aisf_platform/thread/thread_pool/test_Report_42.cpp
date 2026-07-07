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

class TestThreadPoolBase_Report : public ::testing::Test {
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

TEST_F(TestThreadPoolBase_Report, ShouldOutputThreadPoolInfoAndThreadStatus_WhenThreadPoolIsCreatedWithMetricEnabled) {
    // Given
    ThreadPool<TestMessage>::Info info;
    info.name = "TestPool";

    ThreadPoolBase* pool = new ThreadPool<TestMessage>(info);
    stringstream ss;
    // When
    pool->Report(ss);
    // Then
    string output = ss.str();
    EXPECT_NE(output.find("ThreadPool:TestPool, create @"), string::npos);
    EXPECT_EQ(output.find("thread pos:0 last run @ 0"), string::npos);
    EXPECT_EQ(output.find("thread pos:1 last run @ 0"), string::npos);
    EXPECT_EQ(output.find("thread pos:31 last run @ 0"), string::npos);
    // Clean
    delete pool;
}
