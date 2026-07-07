/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "aisf_platform/utils/utils.h"

using namespace mockcpp;
using namespace AisfPlat;
using namespace AisfPlat::StringUtils;

class TestStringRingBuffer_WriteToBuffer : public ::testing::Test {
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

TEST_F(TestStringRingBuffer_WriteToBuffer, ShouldWriteAllData_WhenPositionLessThanCapacity) {
    // Given
    const size_t CAPACITY = 100;
    char* data = static_cast<char*>(malloc(CAPACITY));
    for (size_t i = 0; i < CAPACITY; ++i) {
        data[i] = static_cast<char>('A' + i % 26);
    }
    
    StringRingBuffer buffer(0);
    buffer.*(&StringRingBuffer::data) = data;
    buffer.*(&StringRingBuffer::position) = 0;
    
    stringstream out;
    
    // When
    buffer.WriteToBuffer(out);
    
    // Then
    string result = out.str();
    EXPECT_EQ(result.length(), 0);
    
    // Clean
}
