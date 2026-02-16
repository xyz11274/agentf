#ifndef AGENTFLOW_BASIC_THREAD_POOL_H
#define AGENTFLOW_BASIC_THREAD_POOL_H

#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <memory>

namespace agentflow {
namespace basic {

/**
 * ThreadPool is a generic thread pool implementation
 * for efficient task execution across multiple threads
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    // Disable copy and move
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Submit a task to the thread pool
    // Returns a future that can be used to get the result
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using ReturnType = decltype(f(args...));

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<ReturnType> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }

        condition_.notify_one();
        return result;
    }

    // Get number of worker threads
    size_t get_thread_count() const { return workers_.size(); }

    // Get number of pending tasks
    size_t get_pending_tasks() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    // Check if the pool is running
    bool is_running() const { return running_; }

private:
    void worker_thread_main();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> running_;
};

/**
 * Get the global thread pool instance
 */
ThreadPool& global_thread_pool();

} // namespace basic
} // namespace agentflow

#endif // AGENTFLOW_BASIC_THREAD_POOL_H
