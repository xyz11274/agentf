/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2014-2019. All rights reserved.
 * Description: 一个固定长度的字符串缓存，读写线程安全
 */

#pragma once

#include "safe_string.h"

namespace AisfPlat {
class StringBuffer final {
DISALLOW_COPY_AND_ASSIGN(StringBuffer)

public:
    explicit StringBuffer(const string& name, size_t size = BUFFER_SIZE_1KB);

    ~StringBuffer() = default;

public:
    // 向缓存中写入数据，当缓存中剩余内存不够时，支持扩容。maxCapacity为0，则不期望扩容
    size_t Add(const char* data, size_t size, size_t maxCapacity = 0);

    // 向缓存中写入数据，并以某个字符结尾，当缓存中剩余内存不够时，支持扩容。maxCapacity为0，则不期望扩容
    size_t AddWithEndChar(const char* data, size_t size, char endChar, size_t maxCapacity = 0);

    // 向缓存中写入数据，并以某字符串为抬头，当缓存中剩余内存不够时，支持扩容。maxCapacity为0，则不期望扩容
    size_t AddWithPrefix(const char* prefix, size_t preSize, const char* data, size_t size, size_t maxCapacity = 0);

    // 缓存的初始化。如果多次调用，效果相当于多次Reset
    bool Initial();

    // 缓存是否已经成功初始化
    [[nodiscard]] bool Empty() const;

    // 获取缓存的不可写内部内存指针
    [[nodiscard]] const char* Data() const;

    // 从缓存中的数据取出
    bool Get(SafeString& data);

    // 从缓存中的数据取出，并清空缓存
    bool GetAndReset(SafeString& data);

    // 获取缓存中已经使用的长度
    size_t Size();

    // 获取缓存的可用空间大小
    [[nodiscard]] size_t Capacity();

    // 检查buffer中剩余的内存是否充足，支持扩容
    bool CheckSize(size_t length, size_t maxCapacity);

    // buffer中使用使用的长度清零
    void Reset();

    // 扩容空间，历史数据不会丢失
    bool ResetCapacity(size_t newCapacity);

    // 少于1MB时，按倍扩容，大于1MB时，按原大小的一半扩容，大于8MB时，按原大小的四分之一扩容，大于32MB时，停止扩容，返回失败
    bool AutoCapacity(size_t needLength);

private:
    const string bufferName;

    size_t capacity{0};

    recursive_mutex lock;

    size_t usedSize{0};

    SafeString buffer{"StringBuffer_" + bufferName, __LINE__};
};
}