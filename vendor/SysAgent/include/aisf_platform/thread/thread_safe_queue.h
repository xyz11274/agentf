#pragma once

#include "aisf_platform/utils/utils.h"
#include <condition_variable>
#include <cstdint>
#include <queue>

namespace AisfPlat {
class ThreadSafeQueueMsgBase {
private:
    const static inline int64_t NANO_IN_MINUTE = (int64_t)60 * 1000 * 1000 * 1000;

public:
    const int64_t inQueTimeNano{Utils::GetTimeNano()};
    const int64_t inQueTimeMinPosInHour{(inQueTimeNano / NANO_IN_MINUTE) % MINUTE_IN_HOUR};

public:
    [[nodiscard]] virtual size_t Size() const = 0;
};

class ThreadSafeQueueBase {
    struct Metric {
        const static inline char* HEADER =
            "            Time inQueueCnt inQueueSz succeedCnt succeedSz  failCount  failSize "
            "outQueueCnt outQueueSz avgOutQueue maxOutQueue\n";
        const static inline char* LINE = "%s:%02d %10lu %9s %10lu %9s %10lu %9s %11lu %10s %11s %11s\n";

        uint32_t inQueueCount{0};
        uint64_t inQueueSize{0};

        uint32_t failCount{0};
        uint64_t failSize{0};

        uint32_t succeedCount{0};
        uint64_t succeedSize{0};

        uint32_t outQueueCount{0};
        uint64_t outQueueSize{0};

        uint64_t outWaitTotalNano{0};
        uint64_t maxOutQueueNano{0};
    };

public:
    static void MakeAllReport();

    void Report(stringstream& ss);

protected:
    ThreadSafeQueueBase(string n, bool enableMetric);

    virtual ~ThreadSafeQueueBase();

    virtual void ChildReport(stringstream& out) = 0;

    void SucceedInQueue(const ThreadSafeQueueMsgBase& item);

    void FailedInQueue(const ThreadSafeQueueMsgBase& item);

    void OutQueue(const ThreadSafeQueueMsgBase& item);

protected:
    static inline pair<mutex, unordered_set<ThreadSafeQueueBase*>> instances{};

    const string name;
    const long createTimeMs{Utils::GetTimeMs()};  // 队列创建时间
    const bool isEnableMetric;

    Metric metrics[MINUTE_IN_HOUR]{};
};

// TYPE 类型必须是ThreadSafeQueueMsgBase的子类！！！！
template <typename TYPE>
class ThreadSafeQueue final : public ThreadSafeQueueBase {
    DISALLOW_COPY_AND_ASSIGN(ThreadSafeQueue)

public:
    using type = std::shared_ptr<TYPE>;

    ThreadSafeQueue(const string& name, size_t cap, bool enableMetric)
        : ThreadSafeQueueBase(name, enableMetric),
          capacity(cap)
    {
    }

    ~ThreadSafeQueue() override = default;

    bool Put(const type& item)
    {
        CHECK_NULL_RETURN(item, false);
        {
            std::lock_guard<std::mutex> guard(poolMutex);
            if (pool.size() >= capacity) {
                FailedInQueue(*item);
                return false;
            }
            pool.emplace(item);
            SucceedInQueue(*item);
        }

        poolCondition.notify_one();
        return true;
    }

    // timeoutMs为零则死等
    [[nodiscard]] type Get(uint32_t timeoutMs)
    {
        if (IsEmpty(timeoutMs)) {
            return nullptr;
        }

        std::lock_guard<std::mutex> guard(poolMutex);
        if (pool.empty()) {
            return nullptr;
        }
        auto t = pool.front();
        pool.pop();

        if (t != nullptr) {
            OutQueue(*(t.get()));
        }

        return t;
    }

    bool Puts(const std::vector<type>& items)
    {
        {
            std::lock_guard<std::mutex> guard(poolMutex);
            for (auto& item : items) {
                if (item != nullptr) {
                    if (pool.size() >= capacity) {
                        FailedInQueue(*item);
                        continue;
                    }

                    pool.emplace(item);
                    SucceedInQueue(*item);
                }
            }
        }

        poolCondition.notify_all();
        return true;
    }

    // timeoutMs为零则死等
    [[nodiscard]] auto Gets(size_t count, uint32_t timeoutMs)
    {
        std::vector<type> rst;
        if (IsEmpty(timeoutMs)) {
            return rst;
        }

        std::lock_guard<std::mutex> guard(poolMutex);
        rst.reserve(count);
        for (int i = 0; i < count && !pool.empty(); i++) {
            auto t = pool.front();
            if (t != nullptr) {
                OutQueue(*t);
                rst.emplace_back(t);
            }
            pool.pop();
        }
        return rst;
    }

    [[nodiscard]] size_t Length()
    {
        std::lock_guard<std::mutex> guard(poolMutex);
        return pool.size();
    }

private:
    bool IsEmpty(uint32_t timeoutMs)
    {
        std::unique_lock<std::mutex> lock(poolMutex);
        if (timeoutMs == 0) {
            poolCondition.wait(lock, [this]() { return !pool.empty(); });
        } else {
            // 超时返回为空
            if (!poolCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [this]() { return !pool.empty(); })) {
                return true;
            }
        }

        return pool.empty();
    }

    void ChildReport(stringstream& out) override
    {
        out << ", capacity:" << capacity << ", msg count in queue now:" << Length();
    }

private:
    const size_t capacity;

    mutex conditionMutex;
    std::condition_variable poolCondition;

    mutex poolMutex;
    std::queue<type> pool;
};
}  // namespace AisfPlat