/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;

class TestStringRingBuffer_PlainWrite : public ::testing::Test {
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

TEST_F(TestStringRingBuffer_PlainWrite, ShouldSaveMemoryAndIncreaseLength_WhenValidBufferAndSizeProvided) {
    // Given
    // char* data = new char[100];
    StringRingBuffer ringBuf(100);
    // ringBuf.data = data;
    // ringBuf.length = 0;
    // ringBuf.position = 0;
    std::string buffer = "Hello World! This is a test.";
    size_t iSize = buffer.length();

    // When
    ringBuf.PlainWrite(buffer.c_str(), iSize);

    // Then
    EXPECT_EQ(ringBuf.length, iSize);
    EXPECT_EQ(ringBuf.position, iSize);
    // EXPECT_EQ(memcmp(ringBuf.data, buffer.c_str(), 50), 0);

    // Clean
    // delete[] data;
}

// TEST_F(TestStringRingBuffer_PlainWrite, ShouldNotWrite_WhenBufferIsNullptr) {
//     // Given
//     char* data = new char[100];
//     StringRingBuffer ringBuf(100);
//     ringBuf.data = data;
//     ringBuf.length = 0;
//     ringBuf.position = 0;

//     // When
//     ringBuf.PlainWrite(nullptr, 10);

//     // Then
//     EXPECT_EQ(ringBuf.length, 0);
//     EXPECT_EQ(ringBuf.position, 0);
//     EXPECT_NE(memcmp(ringBuf.data, nullptr, 10), 0);  // Not written

//     // Clean
//     delete[] data;
// }

// TEST_F(TestStringRingBuffer_PlainWrite, ShouldNotWrite_WhenDataIsNullptr) {
//     // Given
//     StringRingBuffer ringBuf(100);
//     ringBuf.data = nullptr;
//     ringBuf.length = 0;
//     ringBuf.position = 0;
//     std::string buffer = "test";

//     // When
//     ringBuf.PlainWrite(buffer.c_str(), 10);

//     // Then
//     EXPECT_EQ(ringBuf.length, 0);
//     EXPECT_EQ(ringBuf.position, 0);

// }

// TEST_F(TestStringRingBuffer_PlainWrite, ShouldWrapAroundAndSaveCorrectly_WhenWriteExceedsCapacity) {
//     // Given
//     char* data = new char[50];
//     StringRingBuffer ringBuf(50);
//     ringBuf.data = data;
//     ringBuf.length = 40;
//     ringBuf.position = 40;
//     std::string buffer = "abcdefghijklmnopqrstuvwxyz";

//     // When
//     ringBuf.PlainWrite(buffer.c_str(), 20);

//     // Then
//     EXPECT_EQ(ringBuf.length, 60);
//     EXPECT_EQ(ringBuf.position, 10);
//     EXPECT_EQ(memcmp(ringBuf.data + 40, buffer.c_str(), 10), 0);  // First part
//     EXPECT_EQ(memcmp(ringBuf.data, buffer.c_str() + 10, 10), 0);  // Second part

//     // Clean
//     delete[] data;
// }

// TEST_F(TestStringRingBuffer_PlainWrite, ShouldHandlePartialWrite_WhenMemcpySReturnsError) {
//     // Given
//     char* data = new char[100];
//     StringRingBuffer ringBuf(100);
//     ringBuf.data = data;
//     ringBuf.length = 0;
//     ringBuf.position = 0;
//     std::string buffer = "Test data for partial write.";

//     // Mock memcpy_s to fail on first call
//     MOCKER(memcpy_s).expects(once()).with(any(), any(), any(), any()).will(returnValue(EINVAL));

//     // When
//     ringBuf.PlainWrite(buffer.c_str(), 60);

//     // Then
//     EXPECT_EQ(ringBuf.length, 0);
//     EXPECT_EQ(ringBuf.position, 0);
//     EXPECT_NE(memcmp(ringBuf.data, buffer.c_str(), 60), 0);  // Not written

//     // Clean
//     delete[] data;
// }

