#pragma once

#include "aisf_platform/common_define.h"

namespace AisfPlat {
// 将一个数据，限定在指定的范围内
template<typename T>
T GetValueRange(const T& value, const T& min, const T& max)
{
    if (value < min) {
        return min;
    }

    if (value > max) {
        return max;
    }

    return value;
}

// 将一个对象用锁保护
template<typename T>
class ThreadSafeObject {
DISALLOW_COPY_AND_ASSIGN(ThreadSafeObject)

public:
    ThreadSafeObject() = default;

    ~ThreadSafeObject() = default;

    [[nodiscard]] T Get()
    {
        _lock.lock_shared();
        auto t = _t;
        _lock.unlock_shared();
        return t;
    }

    void Set(const T& t)
    {
        _lock.lock();
        _t = t;
        _lock.unlock();
    }

private:
    shared_mutex _lock;
    T _t;
};
}