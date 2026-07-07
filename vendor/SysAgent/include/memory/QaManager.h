//
// Created by c00888021 on 2025/12/18.
//

#ifndef SYSAGENT_QAMANAGER_H
#define SYSAGENT_QAMANAGER_H
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <random>
#include "utils/logger.h"
#include "memoryMgr.h"
#include "framework_common/constants/number_constant.h"
#include "aisf_platform/parser/json_parser.h"
#include "utils/http_client.h"
#include "memory/History.h"
#include "utils/common_func.h"
#include "model/ModelInfer.h"
#include <future>
#include <sstream>

using namespace AisfPlat;
using namespace SYSAGENT;

struct QAEntity {
    std::string role;
    std::string message;
    std::string status;
    uint32_t index;
    std::string date;
};

Json::Value GenerateQA(std::string query, std::string answer);

std::tuple<Json::Value, std::string> GenerateAbstractQA(std::string response);

using QaData = std::tuple<std::vector<QAEntity>, std::string, std::string, std::string, std::string>;

class SafeQueue {
public:
    SafeQueue() = default;
    ~SafeQueue() = default;

    // 禁止拷贝和移动（避免线程安全问题）
    SafeQueue(const SafeQueue&) = delete;
    SafeQueue& operator=(const SafeQueue&) = delete;
    SafeQueue(SafeQueue&&) = delete;
    SafeQueue& operator=(SafeQueue&&) = delete;

    // 向队列插入数据（生产）
    void push(const QaData& data)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(data);
        cv_.notify_one();
    }

    // 从队列取出数据（消费），队列为空时阻塞
    bool pop(QaData& data, std::atomic<bool>& stop_flag)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        // 等待条件：队列非空 或 停止标志置位
        cv_.wait(lock, [this, &stop_flag]() {
            return !queue_.empty() || stop_flag.load();
        });

        // 若停止标志置位且队列为空，返回false表示退出
        if (stop_flag.load() && queue_.empty()) {
            return false;
        }

        // 取出队列头部数据
        data = queue_.front();
        queue_.pop();
        return true;
    }

    // 清空队列（可选）
    void clear()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        while (!queue_.empty()) {
            queue_.pop();
        }
    }

    void notifyAll()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cv_.notify_all();
    }

private:
    std::queue<QaData> queue_;         // 底层队列
    std::mutex mtx_;                   // 队列互斥锁
    std::condition_variable cv_;       // 条件变量（用于阻塞/唤醒消费线程）
};

class QAQueueManager {
public:
    static QAQueueManager& GetInstance()
    {
        static QAQueueManager instance;
        return instance;
    }
    QAQueueManager(const QAQueueManager&) = delete;
    QAQueueManager& operator=(const QAQueueManager&) = delete;

    QAQueueManager() : stop_flag_(false)
    {
        // 初始化10个队列和10个消费线程，每个队列绑定一个线程
        for (int i = 0; i < kQueueCount; ++i) {
            queues_.emplace_back(std::make_unique<SafeQueue>());
            threads_.emplace_back(&QAQueueManager::consumeFunc, this, i);
        }
        LOG_INFO_EX("QAQueueManager init finished！");
    }

    ~QAQueueManager()
    {
        stop();
    }

    // 向指定队列插入数据（线程安全）
    void pushToQueue(int queue_idx, const QaData& data)
    {
        if (queue_idx < 0 || queue_idx >= kQueueCount) {
            LOG_INFO_EX("queue_idx is invalid:%d", queue_idx);
            return;
        }
        queues_[queue_idx]->push(data);
        LOG_INFO_EX("add data success!");
    }

    // 停止所有消费线程
    void stop()
    {
        stop_flag_.store(true);
        for (auto& queue : queues_) {
            queue->notifyAll();
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        LOG_INFO_EX("Stop all threads!");
    }

private:
    static constexpr int kQueueCount = 10;           // 队列数量
    std::vector<std::unique_ptr<SafeQueue>> queues_; // 10个安全队列
    std::vector<std::thread> threads_;               // 10个消费线程
    std::atomic<bool> stop_flag_;                    // 线程停止标志（原子变量保证线程安全）

    // 消费线程函数：处理指定队列的数据
    void consumeFunc(int queue_idx);
};

#endif  // SYSAGENT_QAMANAGER_H
