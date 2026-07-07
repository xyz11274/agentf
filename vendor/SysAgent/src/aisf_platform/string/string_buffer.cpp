#include "aisf_platform/utils/utils.h"

namespace AisfPlat {
StringBuffer::StringBuffer(const string& name, size_t size) : bufferName(name), capacity(size)
{
}

size_t StringBuffer::Add(const char* data, size_t size, size_t maxCapacity)
{
    CHECK_TRUE_RETURN(data == nullptr || size == 0, 0);

    recursive_mutex_guard guard(lock);

    if (buffer.Empty()) {
        CHECK_FALSE_RETURN(Initial(), 0);
    }

    if (maxCapacity != 0) {
        CHECK_FALSE_RETURN(CheckSize(size, maxCapacity), 0);
    }

    auto leftSize = usedSize >= capacity ? 0 : capacity - usedSize;
    CHECK_TRUE_RETURN(leftSize == 0, 0); // buffer满了

    auto writeSize = std::min(size, leftSize);
    auto ret = memcpy_s(buffer.Get() + usedSize, leftSize, data, writeSize);
    CHECK_TRUE_RETURN(ret != EOK, 0);
    usedSize += writeSize;

    return writeSize;
}

size_t StringBuffer::AddWithEndChar(const char* data, size_t size, char endChar, size_t maxCapacity)
{
    CHECK_TRUE_RETURN(data == nullptr || size == 0, 0);

    recursive_mutex_guard guard(lock);

    auto writeSize = Add(data, size, maxCapacity);
    CHECK_TRUE_RETURN(writeSize != size, writeSize);

    if (data[size - 1] != endChar && usedSize < capacity) { // 不是换行结尾就加上换行
        buffer.Get()[usedSize] = endChar;
        usedSize += 1;
    }

    return writeSize;
}

size_t StringBuffer::AddWithPrefix(const char* prefix, size_t preSize, const char* data, size_t size,
                                   size_t maxCapacity)
{
    CHECK_TRUE_RETURN(data == nullptr || size == 0, 0);

    recursive_mutex_guard guard(lock);

    auto writePreSize = Add(prefix, preSize, maxCapacity);
    CHECK_TRUE_RETURN(writePreSize != preSize, writePreSize);

    auto writeSize = AddWithEndChar(data, size, '\n', maxCapacity);

    return writePreSize + writeSize;
}

bool StringBuffer::Initial()
{
    recursive_mutex_guard guard(lock);

    usedSize = 0;

    const static int BUFFER_RESERVE = 32;
    if (buffer.Size() <= BUFFER_RESERVE || buffer.Size() - BUFFER_RESERVE != capacity) {
        buffer.Reset(capacity + BUFFER_RESERVE); // 预留一部分数据，防止写超
    }

    return !buffer.Empty();
}

bool StringBuffer::Empty() const
{
    return buffer.Empty();
}

const char* StringBuffer::Data() const
{
    return buffer.Data();
}

size_t StringBuffer::Size()
{
    recursive_mutex_guard guard(lock);
    return usedSize;
}

void StringBuffer::Reset()
{
    recursive_mutex_guard guard(lock);
    usedSize = 0;
}

size_t StringBuffer::Capacity()
{
    recursive_mutex_guard guard(lock);
    return capacity;
}

bool StringBuffer::CheckSize(size_t length, size_t maxCapacity)
{
    CHECK_TRUE_RETURN(maxCapacity == 0, true); // 不期望扩容

    recursive_mutex_guard guard(lock);

    if (Capacity() - Size() <= length) {
        if (Capacity() >= maxCapacity) {
            // 空间已经不够了，且无法扩充
            return false;
        }

        // 扩充空间
        return ResetCapacity(maxCapacity);
    }

    return true;
}

bool StringBuffer::Get(SafeString& data)
{
    recursive_mutex_guard guard(lock);

    CHECK_TRUE_RETURN(buffer.Empty(), false);

    data.Reset(usedSize);
    CHECK_TRUE_RETURN(data.Empty(), false);

    return data.Write(buffer.Data(), usedSize);
}

bool StringBuffer::GetAndReset(SafeString& data)
{
    CHECK_FALSE_RETURN(Get(data), false);
    Reset();
    return true;
}

bool StringBuffer::ResetCapacity(size_t newCapacity)
{
    recursive_mutex_guard guard(lock);

    CHECK_TRUE_RETURN(newCapacity <= capacity, true);

    SafeString data{__FUNCTION__, __LINE__};
    if (usedSize != 0) {
        // 获取老数据
        CHECK_FALSE_RETURN(Get(data), false);
    }

    capacity = newCapacity;
    CHECK_FALSE_RETURN(Initial(), false);

    if (!data.Empty()) {
        auto ret = memcpy_s(buffer.Get(), buffer.Size(), data.Data(), data.Size());
        CHECK_TRUE_RETURN(ret != EOK, false);
        usedSize = data.Size();
    }

    return true;
}

bool StringBuffer::AutoCapacity(size_t needLength)
{
    recursive_mutex_guard guard(lock);

    auto left = usedSize >= capacity ? 0 : capacity - usedSize;
    CHECK_TRUE_RETURN(left >= needLength, true);

    // 需要扩容了 少于1MB时，按倍扩容，大于1MB时，按原大小的一半扩容，大于8MB时，按原大小的四分之一扩容，大于32MB时，停止扩容，返回失败
    size_t newCap = 0;
    if (capacity < BUFFER_SIZE_1MB) {
        newCap = capacity + capacity + needLength;
    } else if (capacity < BUFFER_SIZE_8MB) {
        newCap = capacity + capacity / HALF + needLength;
    } else if (capacity < BUFFER_SIZE_8MB * QUARTER) {
        newCap = capacity + capacity / QUARTER + needLength;
    } else {
        return false; // 内存超大，超过32MB，不扩容了
    }

    return ResetCapacity(newCap);
}
}