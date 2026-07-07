#pragma once

#include "aisf_platform/utils/utils.h"
#include "thread_safe_queue.h"
#include <thread>
#include <functional>

namespace AisfPlat {

class ThreadPoolBase {
protected:
    const inline static uint16_t MAX_THREAD_COUNT = 32; // 最大线程个数是32个！！！

public:
    static void MakeAllReport();

    void Report(stringstream& ss);

protected:
    ThreadPoolBase(string name, bool enableMetric);

    virtual ~ThreadPoolBase();

    virtual void ChildReport(stringstream& out) = 0;

protected:
    static inline pair<mutex, unordered_set<ThreadPoolBase*>> instances{};

    const string m_name;
    const long m_createTimeMs{Utils::GetTimeMs()}; // 创建时间
    const bool m_isEnableMetric;

    atomic<long> m_lastProcTimeStamp[MAX_THREAD_COUNT]{}; // 上次处理时间，如果过长，说明线程阻塞
};

template<typename TYPE>
class ThreadPool final : public ThreadPoolBase {
DISALLOW_COPY_AND_ASSIGN(ThreadPool)

    typedef void (* Callback)(void* cbPara, const TYPE& msg);

public:
    struct Info { // 线程池的配置参数
        string name; // 线程池的名字
        uint32_t queueCap{0}; // 线程池内部队列的容量
        uint16_t threadCnt{0}; // 线程池的线程个数

        Callback callback{nullptr}; // 工作线程调用的回调函数
        void* cbPara{nullptr}; // 回调函数的自定义入参
        bool enableMetric{false}; // 是否开启内部队列的计数功能，用于维测，开启后，性能会降低
    };

    explicit ThreadPool(const Info& info) :
        ThreadPoolBase(info.name, info.enableMetric), m_threadCnt(info.threadCnt), m_callback(info.callback),
        m_cbPara(info.cbPara), m_queue(info.name, info.queueCap, info.enableMetric) {}

    ~ThreadPool() override = default;

    bool Start()
    {
        CHECK_TRUE_RETURN(m_isRunning->load(), true);
        CHECK_NULL_LOG_ERROR_RETURN(m_callback, false, "callback in nullptr");
        CHECK_TRUE_LOG_ERROR_RETURN(m_threadCnt == 0, false, "threadCnt is zero");
        CHECK_TRUE_LOG_ERROR_RETURN(m_threadCnt > MAX_THREAD_COUNT, false, "threadCnt:%u too big", m_threadCnt);

        m_isRunning->store(true);
        for (uint16_t i = 0; i < m_threadCnt; i++) {
            std::thread t(std::bind(&ThreadPool::Run, this, i));
            t.detach();
        }

        return true;
    }

    [[nodiscard]] bool IsRunning() const { return m_isRunning->load(); }

    void Stop() { m_isRunning->store(false); }

    bool Put(const shared_ptr<TYPE>& item) { return m_queue.Put(item); }

    bool Puts(const vector<std::shared_ptr<TYPE>>& items) { return m_queue.Puts(items); }

private:
    void Run(uint16_t pos)
    {
        CHECK_TRUE_LOG_ERROR_RETURN_VOID(pos >= MAX_THREAD_COUNT, "pos %d exceed max", pos);

        const auto threadName = m_name + "_" + std::to_string(pos);
        Utils::SetThreadName(threadName);
        LOG_ERR("thread m_pool:%s start...", threadName.c_str(), pos);

        auto callback = m_callback;
        CHECK_NULL_RETURN_VOID(callback);

        auto isRunning = m_isRunning;
        while (isRunning->load()) {
            auto msg = m_queue.Get(MILLI_IN_SECOND);
            CHECK_FALSE_RETURN_VOID(isRunning->load());
            CHECK_TRUE_CONTINUE(msg == nullptr) // 不可能存在，代码防呆
            callback(m_cbPara, *msg);
            m_lastProcTimeStamp[pos] = Utils::GetTimeMs();
        }

        LOG_ERR("thread m_pool:%s exit", threadName.c_str());
    }

    void ChildReport(stringstream& out) override
    {
        if (!m_isRunning->load()) {
            out << "pool not start yet!" << std::endl;
            return;
        }
        out << "threadCnt:" << m_threadCnt << ", callback exist:" << BOOL2STR(m_callback != nullptr)
            << ", cbPara exist:" << BOOL2STR(m_cbPara != nullptr) << std::endl;
        m_queue.Report(out);
    }

private:
    const uint16_t m_threadCnt;
    const Callback m_callback;
    void* m_cbPara;

    shared_ptr<atomic_bool> m_isRunning{make_shared<atomic_bool>(false)};
    ThreadSafeQueue<TYPE> m_queue;
};
}