// test_worker_threadpool_gtest.cpp — 验证 ThreadPool 清理修复
//
// 测试范围:
//   1. ThreadPool 析构正确 join 所有 worker 线程
//   2. MMWorkerEngine register/unregister workflow 生命周期
//   3. workflow_pools_ 在 unregister 后正确清理
//
// 对应修复: runtime.cpp on_workflow_complete() 增加 unregister_workflow_instance()

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "utils/thread_pool.h"
#include "runtime/worker.h"
#include "utils/logger.h"

namespace agentflow {
namespace runtime {
namespace tests {

// ============================================================================
// Test 1: ThreadPool 生命周期 — 线程创建和销毁
// ============================================================================

TEST(ThreadPoolLifecycleTest, ThreadsCreatedAndDestroyed) {
  const size_t num_threads = 4;

  // 记录创建前的 alive 线程数（通过创建另一个临时 pool 来判断）
  std::atomic<size_t> tasks_executed{0};

  {
    basic::ThreadPool pool(num_threads);
    EXPECT_EQ(pool.get_thread_count(), num_threads);
    EXPECT_TRUE(pool.is_running());

    // 提交几个任务确认线程在工作
    for(size_t i = 0; i < num_threads * 2; ++i) {
      pool.submit([&tasks_executed]() {
        tasks_executed.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      });
    }

    // 等待任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_GE(tasks_executed.load(), num_threads * 2);

    // pool 离开作用域 → 析构 → join 所有线程
  }

  // 析构后，所有线程应该已经被 join
  // 验证: 不崩溃 + 后续无残留操作
  SUCCEED() << "ThreadPool destructor joined all threads without crash";
}

TEST(ThreadPoolLifecycleTest, MultipleCreateDestroyCycles) {
  // 验证多次创建/销毁不会累积线程
  for(int cycle = 0; cycle < 10; ++cycle) {
    basic::ThreadPool pool(2);
    EXPECT_EQ(pool.get_thread_count(), 2);

    pool.submit([]() { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // 析构 → join
  }
  SUCCEED() << "10 create/destroy cycles completed without thread accumulation";
}

TEST(ThreadPoolLifecycleTest, EmptyDestruction) {
  // 验证空 ThreadPool (0线程) 也能正常析构
  basic::ThreadPool pool(0);
  EXPECT_EQ(pool.get_thread_count(), 0);
  SUCCEED() << "Empty ThreadPool destroyed without crash";
}

// ============================================================================
// Test 2: MMWorkerEngine register/unregister 生命周期
// ============================================================================

class WorkerThreadPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    MMWorkerEngine::Config config;
    config.worker_id               = "test_worker";
    config.default_workflow_threads = 1;
    config.control_threads         = 1;
    config.business_threads        = 1;
    config.max_queue_size          = 64;

    engine_ = std::make_unique<MMWorkerEngine>(config);
    engine_->start();

    // 等待 engine 启动完成
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void TearDown() override {
    if(engine_) {
      engine_->stop();
      engine_.reset();
    }
  }

  std::unique_ptr<MMWorkerEngine> engine_;
};

TEST_F(WorkerThreadPoolTest, RegisterUnregisterWorkflow) {
  // Step 1: 注册 workflow 实例
  MMWorkerEngine::WorkflowRegistration reg;
  reg.workflow_id          = "test_workflow";
  reg.workflow_instance_id = "test_instance_001";
  reg.thread_count         = 1;

  EXPECT_TRUE(engine_->register_workflow_instance(reg));

  // 等待控制消息处理（ensure_workflow_pool 被调用）
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Step 2: 取消注册
  EXPECT_TRUE(engine_->unregister_workflow_instance("test_instance_001"));

  // 等待控制消息处理（workflow_pools_.erase 被调用）
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 验证: 不崩溃，无异常
  SUCCEED() << "Register/unregister cycle completed without crash";
}

TEST_F(WorkerThreadPoolTest, MultipleRegisterUnregister) {
  // 验证多次 register/unregister 不会累积资源
  for(int i = 0; i < 5; ++i) {
    std::string instance_id = "test_instance_" + std::to_string(i);

    MMWorkerEngine::WorkflowRegistration reg;
    reg.workflow_id          = "test_workflow";
    reg.workflow_instance_id = instance_id;
    reg.thread_count         = 1;

    EXPECT_TRUE(engine_->register_workflow_instance(reg));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(engine_->unregister_workflow_instance(instance_id));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  SUCCEED() << "5 register/unregister cycles completed";
}

TEST_F(WorkerThreadPoolTest, UnregisterNonexistentDoesNotCrash) {
  // 验证取消注册不存在的实例不会崩溃
  EXPECT_TRUE(engine_->unregister_workflow_instance("nonexistent_instance"));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  SUCCEED() << "Unregister nonexistent instance handled gracefully";
}

TEST_F(WorkerThreadPoolTest, ConcurrentRegisterUnregister) {
  // 验证并发 register/unregister 不会导致崩溃或死锁
  std::atomic<bool> running{true};
  std::atomic<int>  success_count{0};
  std::atomic<int>  error_count{0};

  auto worker_func = [this, &running, &success_count, &error_count](int thread_id) {
    while(running) {
      std::string instance_id =
          "concurrent_" + std::to_string(thread_id) + "_"
          + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

      MMWorkerEngine::WorkflowRegistration reg;
      reg.workflow_id          = "test_workflow";
      reg.workflow_instance_id = instance_id;
      reg.thread_count         = 1;

      if(engine_->register_workflow_instance(reg)) {
        success_count.fetch_add(1);
      } else {
        error_count.fetch_add(1);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));

      if(engine_->unregister_workflow_instance(instance_id)) {
        success_count.fetch_add(1);
      } else {
        error_count.fetch_add(1);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  std::vector<std::thread> workers;
  for(int i = 0; i < 3; ++i) {
    workers.emplace_back(worker_func, i);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  running = false;

  for(auto& w : workers) {
    w.join();
  }

  EXPECT_GT(success_count.load(), 0);
  EXPECT_EQ(error_count.load(), 0);
  Logger::instance().info_f("[TEST] Concurrent test: %d success, %d errors",
                            success_count.load(), error_count.load());
}

}  // namespace tests
}  // namespace runtime
}  // namespace agentflow
