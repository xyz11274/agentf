#pragma once

#include "aisf_platform/common_define.h"

namespace AisfPlat {
template<typename KEY, typename VALUE>
class ThreadSafeMap final {
DISALLOW_COPY_AND_ASSIGN(ThreadSafeMap)

public:
    ThreadSafeMap() = default;

    ~ThreadSafeMap() = default;

    // 加入一对数据
    void Put(const KEY& key, const VALUE& value)
    {
        shared_mutex_write_guard guard(lock);
        data[key] = value;
    }

    // 清理数据
    void Clear()
    {
        shared_mutex_write_guard guard(lock);
        data.clear();
    }

    // 删除一个数据
    void Erase(const KEY& key)
    {
        shared_mutex_write_guard guard(lock);
        data.erase(key);
    }

    // 是否为空
    [[nodiscard]] bool Empty()
    {
        shared_mutex_read_guard guard(lock);
        return data.empty();
    }

    // 数据的个数
    [[nodiscard]] size_t Size()
    {
        shared_mutex_read_guard guard(lock);
        return data.size();
    }

    // key是否存在
    [[nodiscard]] bool Exist(const KEY& key)
    {
        shared_mutex_read_guard guard(lock);
        return data.find(key) != data.end();
    }

    // 查询key的数据
    [[nodiscard]] bool Get(const KEY& key, VALUE& value)
    {
        shared_mutex_read_guard guard(lock);
        auto itr = data.find(key);
        if (itr != data.end()) {
            value = itr->second;
            return true;
        }

        return false;
    }

    // 获取key列表
    [[nodiscard]] vector<KEY> Keys()
    {
        shared_mutex_read_guard guard(lock);

        vector<KEY> keys;
        keys.reserve(data.size());
        for (const auto& p : data) {
            keys.push_back(p.first);
        }

        return keys;
    }

    // 获取value列表
    [[nodiscard]] vector<VALUE> Values()
    {
        shared_mutex_read_guard guard(lock);

        vector<KEY> values;
        values.reserve(data.size());
        for (const auto& p : data) {
            values.push_back(p.second);
        }

        return values;
    }

    // 拷贝一份内部数据
    [[nodiscard]] auto Data()
    {
        shared_mutex_read_guard guard(lock);
        return data;
    }

private:
    map<KEY, VALUE> data{};
    shared_mutex lock{};
};
}