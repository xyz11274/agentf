#include "agentflow/basic/thread_pool.h"
#include "agentflow/basic/logger.h"
#include <iostream>

namespace agentflow {
namespace basic {

ThreadPool::ThreadPool(size_t num_threads)
    : running_(true) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_thread_main, this);
    }

    Logger::instance().info_f("ThreadPool initialized with %zu worker threads",
                             num_threads);
}

ThreadPool::~ThreadPool() {
    running_ = false;
    condition_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    Logger::instance().info("ThreadPool shutdown complete");
}

void ThreadPool::worker_thread_main() {
    while (running_) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // Wait for task or shutdown
            condition_.wait(lock, [this]() {
                return !tasks_.empty() || !running_;
            });

            if (!running_) {
                break;
            }

            if (tasks_.empty()) {
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // Execute task outside the lock
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                Logger::instance().error_f("ThreadPool task exception: %s",
                                         e.what());
            } catch (...) {
                Logger::instance().error("ThreadPool task: unknown exception");
            }
        }
    }
}

namespace {
// Global thread pool instance
std::unique_ptr<ThreadPool> g_global_thread_pool;
std::mutex g_global_pool_mutex;
}

ThreadPool& global_thread_pool() {
    std::lock_guard<std::mutex> lock(g_global_pool_mutex);

    if (!g_global_thread_pool) {
        // Create global thread pool with default number of threads
        // Use hardware concurrency or default to 4 threads
        size_t num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 4;
        }
        g_global_thread_pool = std::make_unique<ThreadPool>(num_threads);
    }

    return *g_global_thread_pool;
}

} // namespace basic
} // namespace agentflow
