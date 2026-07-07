/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */
//
// Created by z00913804 on 25-12-8.
//

#ifndef SYSAGENT_THREAD_SAFE_VECTOR_H
#define SYSAGENT_THREAD_SAFE_VECTOR_H
#include <vector>
#include <shared_mutex>
#include <stdexcept>
#include <functional>
template<typename T>
class ThreadSafeVector {
public:
    void push_back(const T &value)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        data_.push_back(value);
    }

    const T &at(size_t index) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (index >= data_.size()) {
            throw std::out_of_range("Index out of range");
        }
        return data_[index];
    }

    void write(size_t index, const T &value)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (index >= data_.size()) {
            throw std::out_of_range("Index out of range");
        }
        data_[index] = value;
    }

    size_t size() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.size();
    }

    bool empty() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.empty();
    }

    void clear()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        data_.clear();
    }

    void forEach(const std::function<void(const T &)> &func) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const T &item: data_) {
            func(item);
        }
    }

    const T* findFirst(const std::function<bool(const T&)>& predicate) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const T& item : data_) {
            if (predicate(item)) {
                return &item;
            }
        }
        return nullptr;
    }

    T* findFirst(const std::function<bool(const T&)>& predicate)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (T& item : data_) {
            if (predicate(item)) {
                return &item;
            }
        }
        return nullptr;
    }

    bool removeIf(const std::function<bool(const T&)>& predicate)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto it = data_.begin(); it != data_.end(); ++it) {
            if (predicate(*it)) {
                data_.erase(it);
                return true;
            }
        }
        return false;
    }

private:
    std::vector<T> data_;
    mutable std::shared_mutex mutex_; // mutable 允许 const 方法中加锁
};
#endif // SYSAGENT_THREAD_SAFE_VECTOR_H
