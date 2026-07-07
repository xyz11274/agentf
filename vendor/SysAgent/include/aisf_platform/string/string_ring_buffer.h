/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2014-2019. All rights reserved.
 * Description: 一个环回内存buffer，新的数据覆盖老的数据
 */

#pragma once

#include "aisf_platform/common_define.h"
#include <sys/time.h>

namespace AisfPlat {
class StringRingBuffer final {
public:
    explicit StringRingBuffer(unsigned size);

    ~StringRingBuffer();

    // 将内存buffer中的数据写入输出对象
    void WriteToBuffer(stringstream& out);

    // 获取环回内存buffer总写入量
    [[nodiscard]] size_t GetTotalWrite();

    // 写数据
    void PlainWrite(const char* buffer, size_t iSize);

    // 返回buffer的容量
    [[nodiscard]] size_t Capacity() const;

    // 获取当前所有环回内存buffer总内存消耗
    [[nodiscard]] static size_t TotalLength();

private:
    void SaveMemory(const char* buffer, size_t iSize);

private:
    recursive_mutex lock;

    char* data;
    size_t length;
    size_t position;
    const size_t capacity;

    static atomic_ullong totalLength;
};
}