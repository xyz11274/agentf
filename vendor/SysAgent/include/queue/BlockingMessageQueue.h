#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class BlockingMessageQueue {
public:

    T GetMessage() {
        std::unique_lock<std::mutex> lock(queueMutex);
        condition.wait(lock, [this] { return !messageQueue.empty(); });  // 等待消息
        T msg = messageQueue.front();
        messageQueue.pop();
        return msg;
    }
    // 批量取出消息，最多取 maxCount 条， 或者一定ms后超时
    std::vector<T> GetMessages(size_t maxCount, std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
    {
        std::vector<T> result;
        std::unique_lock<std::mutex> lock(queueMutex);
        if (messageQueue.empty()) {
            // 队列为空，等待 timeout 时间
            if (timeout > std::chrono::milliseconds::zero()) {
                condition.wait_for(lock, timeout);
            } else {
                // 如果 timeout 为 0，等到非空
                condition.wait(lock, [this] { return !messageQueue.empty();}); // 永远不满足，无限等待
            }
        }
        // 队列不为空，或已经等待超时时间，取最多 maxCount 条
        while (result.size() < maxCount && !messageQueue.empty()) {
            result.push_back(messageQueue.front());
            messageQueue.pop();
        }
        return result;
    }
    void PostMessage(const T &msg) {
        std::lock_guard<std::mutex> lock(queueMutex);
        messageQueue.push(msg);
        condition.notify_one();  // 通知等待的线程
    }

private:
    std::queue<T> messageQueue;  // 消息队列
    std::mutex queueMutex;             // 保护队列的互斥锁
    std::condition_variable condition; // 条件变量用于阻塞和通知
};