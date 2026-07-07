/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringRingBuffer_SaveMemory : public ::testing::Test {
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

TEST_F(TestStringRingBuffer_SaveMemory, ShouldWriteDataSuccessfully_WhenBufferSizeLessThanLeftBufferLength) {
    // Given
    // char* data = new char[100];
    char* data = static_cast<char*>(malloc(100));
    memset(data, 0, 100);

    StringRingBuffer ringBuf(0);
    ringBuf.*(&StringRingBuffer::data) = data;
    ringBuf.*(&StringRingBuffer::position) = 0;
    ringBuf.*(&StringRingBuffer::length) = 0;
    const char* buffer = "Hello World! This is a test buffer.";
    size_t iSize = 30;

    // When
    ringBuf.SaveMemory(buffer, iSize);

    // Then
    EXPECT_EQ(ringBuf.*(&StringRingBuffer::position), 30);
    EXPECT_EQ(memcmp(data, buffer, 30), -1);

    // Clean
}