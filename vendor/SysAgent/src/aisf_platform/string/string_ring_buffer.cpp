#include "aisf_platform/utils/utils.h"

namespace AisfPlat {
atomic_ullong StringRingBuffer::totalLength(0);

StringRingBuffer::StringRingBuffer(unsigned size)
    : data(nullptr), length(0), position(0), capacity(size > BUFFER_SIZE_8MB ? BUFFER_SIZE_8MB : size)
{
    CHECK_TRUE_RETURN_VOID(capacity == 0);
    data = (char*) malloc(capacity + 1);
    if (data != nullptr) {
        auto iSecFuncRet = memset_s(data, capacity + 1, 0, capacity + 1);
        CHECK_TRUE_LOG_ERROR(iSecFuncRet != EOK, "memset_s error");
    }

    totalLength += capacity + 1;
}

StringRingBuffer::~StringRingBuffer()
{
    recursive_mutex_guard guard(lock);
    if (nullptr != data) {
        free(data);
        data = nullptr;
    }
}

void StringRingBuffer::WriteToBuffer(stringstream& out)
{
    CHECK_TRUE_RETURN_VOID(data == nullptr);

    recursive_mutex_guard guard(lock);

    auto pos = position % capacity;
    if (position > capacity) { // 已经写过一轮了，先写后半截的
        out.write(data + pos, capacity - pos);
    }

    if (pos != 0) { // 写前半截的内存
        out.write(data, pos);
    }
}

size_t StringRingBuffer::GetTotalWrite()
{
    recursive_mutex_guard guard(lock);
    return length;
}

void StringRingBuffer::PlainWrite(const char* buffer, size_t iSize)
{
    CHECK_TRUE_RETURN_VOID(buffer == nullptr);
    CHECK_TRUE_RETURN_VOID(data == nullptr);

    SaveMemory(buffer, iSize);
    length += iSize;
}

void StringRingBuffer::SaveMemory(const char* buffer, size_t iSize)
{
    recursive_mutex_guard guard(lock);
    size_t startWritePos = position % capacity;
    size_t leftBufferLen = capacity - startWritePos;

    if (iSize > leftBufferLen) {
        auto iSecFuncRet = memcpy_s(data + startWritePos, leftBufferLen, buffer, leftBufferLen);
        CHECK_TRUE_LOG_ERROR(iSecFuncRet != EOK, "memcpy_s error");
        iSecFuncRet = memcpy_s(data, capacity, buffer + leftBufferLen, iSize - leftBufferLen);
        CHECK_TRUE_LOG_ERROR(iSecFuncRet != EOK, "memcpy_s error");
    } else {
        auto iSecFuncRet = memcpy_s(data + startWritePos, leftBufferLen, buffer, (size_t) iSize);
        CHECK_TRUE_LOG_ERROR(iSecFuncRet != EOK, "memcpy_s error");
    }

    position += iSize;
}

size_t StringRingBuffer::Capacity() const
{
    return capacity;
}

size_t StringRingBuffer::TotalLength()
{
    return totalLength;
}
}