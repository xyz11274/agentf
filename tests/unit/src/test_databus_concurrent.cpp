// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Concurrent DataBus tests: thread safety, data_ref integrity, session isolation.
//
// Run with --gtest_filter=DataBusConcurrent* (or --gtest_filter=DataBusConcurrent*.Concurrent*
// to skip RemoteDatabus tests that require a Redis endpoint).
//
// ThreadSanitizer usage:
//   cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -O1" && make && ./test_databus_concurrent

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/databus/databus.h"
#include "runtime/databus/node_databus_client.h"
#include "runtime/databus/node_databus_adapter.h"

namespace agentflow {
namespace runtime {
namespace {

// ============================================================================
// Helpers
// ============================================================================

/// Simple barrier for N threads (C++17 compatible, no std::barrier).
class ThreadBarrier {
public:
  explicit ThreadBarrier(int count) : count_(count), initial_(count) {}

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    --count_;
    if (count_ == 0) {
      cv_.notify_all();
    } else {
      cv_.wait(lock, [this] { return count_ == 0; });
    }
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    count_ = initial_;
  }

private:
  int count_;
  int initial_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// ============================================================================
// Test 1: 多线程并发 Put/Get 无数据丢失
//
// N threads each write M unique key-value pairs, then verify all keys are
// retrievable with the correct value. No keys overlap between threads.
// ============================================================================

class DataBusConcurrentPutGetTest : public ::testing::Test {
protected:
  static constexpr int kNumThreads = 4;
  static constexpr int kKeysPerThread = 100;

  MMDataBus databus_{1024 * 1024};
};

TEST_F(DataBusConcurrentPutGetTest, NoDataLossUnderConcurrentWrites) {
  ThreadBarrier barrier(kNumThreads + 1);  // +1 for the main/verifier thread
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  for (int tid = 0; tid < kNumThreads; ++tid) {
    writers.emplace_back([&, tid]() {
      barrier.wait();  // all writers start simultaneously

      for (int i = 0; i < kKeysPerThread; ++i) {
        std::string key = "concurrent_k" + std::to_string(tid) + "_" + std::to_string(i);
        std::string val = "v" + std::to_string(tid * kKeysPerThread + i);
        if (!databus_.put(key, DataVariant(val))) {
          failures.fetch_add(1);
        }
      }
    });
  }

  barrier.wait();  // release all writers

  for (auto& t : writers) {
    t.join();
  }

  ASSERT_EQ(failures.load(), 0) << "Some put() calls failed under concurrency";

  // Verify every key
  for (int tid = 0; tid < kNumThreads; ++tid) {
    for (int i = 0; i < kKeysPerThread; ++i) {
      std::string key = "concurrent_k" + std::to_string(tid) + "_" + std::to_string(i);
      std::string expected = "v" + std::to_string(tid * kKeysPerThread + i);

      DataVariant actual;
      ASSERT_TRUE(databus_.get(key, actual))
          << "Missing key: " << key << " (expected: " << expected << ")";
      ASSERT_TRUE(std::holds_alternative<std::string>(actual))
          << "Wrong type for key: " << key;
      EXPECT_EQ(std::get<std::string>(actual), expected)
          << "Corrupted value for key: " << key;
    }
  }

  // Verify total data count
  EXPECT_EQ(databus_.get_data_count(), kNumThreads * kKeysPerThread);
}

TEST_F(DataBusConcurrentPutGetTest, ConcurrentPutGetSameKeysNoCorruption) {
  // Multiple threads hammer the same set of keys. Final values must be
  // internally consistent (any writer's value, never a mix/garbled).
  static constexpr int kNumKeys = 10;
  static constexpr int kWritesPerKey = 50;

  ThreadBarrier barrier(kNumThreads + 1);
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  for (int tid = 0; tid < kNumThreads; ++tid) {
    writers.emplace_back([&, tid]() {
      barrier.wait();

      for (int w = 0; w < kWritesPerKey; ++w) {
        for (int k = 0; k < kNumKeys; ++k) {
          std::string key = "shared_k" + std::to_string(k);
          std::string val = "writer" + std::to_string(tid) + "_w" + std::to_string(w);
          if (!databus_.put(key, DataVariant(val))) {
            failures.fetch_add(1);
          }
        }
      }
    });
  }

  barrier.wait();

  for (auto& t : writers) {
    t.join();
  }

  ASSERT_EQ(failures.load(), 0);

  // After all writes, each key must hold a complete, well-formed value
  // (any writer's final write wins — which is fine; what matters is no
  // corruption, i.e. value must be parseable as "writer<N>_w<M>").
  for (int k = 0; k < kNumKeys; ++k) {
    std::string key = "shared_k" + std::to_string(k);
    DataVariant actual;
    ASSERT_TRUE(databus_.get(key, actual)) << "Missing key: " << key;
    ASSERT_TRUE(std::holds_alternative<std::string>(actual));
    std::string val = std::get<std::string>(actual);
    // Must start with "writer" and contain "_w"
    EXPECT_TRUE(val.rfind("writer", 0) == 0) << "Corrupted value: " << val << " for key: " << key;
    EXPECT_NE(val.find("_w"), std::string::npos) << "Corrupted value: " << val << " for key: " << key;
  }
}

// ============================================================================
// Test 2: data_ref 不被悬挂（local mode, no Redis required）
//
// store_with_ref 写入后即使并发操作底层数据，resolve_ref 仍能解析到完整值。
// 远程 DataBus + owner index 的测试需要 Redis，convention 跳过 CI 无 Redis 场景。
// ============================================================================

class DataBusConcurrentDataRefTest : public ::testing::Test {
protected:
  static constexpr int kNumThreads = 4;
  static constexpr int kRefsPerThread = 50;

  MMDataBus databus_{2 * 1024 * 1024};
};

TEST_F(DataBusConcurrentDataRefTest, ConcurrentStoreResolveDataRef) {
  ThreadBarrier barrier(kNumThreads + 1);
  std::atomic<int> store_failures{0};
  std::vector<std::string> all_addresses;
  std::mutex addr_mutex;

  std::vector<std::thread> writers;
  for (int tid = 0; tid < kNumThreads; ++tid) {
    writers.emplace_back([&, tid]() {
      barrier.wait();

      for (int i = 0; i < kRefsPerThread; ++i) {
        MMDataBus::OwnerContext owner;
        owner.request_id = "req-concurrent-" + std::to_string(tid);
        owner.session_id = "session-" + std::to_string(tid);

        MMDataBus::DataRef ref;
        std::string payload = "payload_" + std::to_string(tid) + "_" + std::to_string(i);
        std::string key = "ref_key_" + std::to_string(tid) + "_" + std::to_string(i);

        if (!databus_.store_with_ref(key, DataVariant(payload), owner, ref)) {
          store_failures.fetch_add(1);
          continue;
        }

        {
          std::lock_guard<std::mutex> lock(addr_mutex);
          all_addresses.push_back(ref.address);
        }
      }
    });
  }

  barrier.wait();

  for (auto& t : writers) {
    t.join();
  }

  ASSERT_EQ(store_failures.load(), 0) << "Some store_with_ref failed";

  // Resolve every ref concurrently (simulate multiple readers racing)
  ThreadBarrier resolve_barrier(kNumThreads + 1);
  std::atomic<int> resolve_failures{0};
  std::mutex error_mutex;
  std::vector<std::string> dangling_refs;

  std::vector<std::thread> readers;
  int refs_per_reader = static_cast<int>(all_addresses.size()) / kNumThreads;

  for (int tid = 0; tid < kNumThreads; ++tid) {
    readers.emplace_back([&, tid]() {
      int start = tid * refs_per_reader;
      int end = (tid == kNumThreads - 1)
                    ? static_cast<int>(all_addresses.size())
                    : start + refs_per_reader;

      resolve_barrier.wait();

      for (int idx = start; idx < end; ++idx) {
        DataVariant resolved;
        if (!databus_.resolve_ref(all_addresses[idx], resolved)) {
          resolve_failures.fetch_add(1);
          std::lock_guard<std::mutex> lock(error_mutex);
          dangling_refs.push_back(all_addresses[idx]);
          continue;
        }
        ASSERT_TRUE(std::holds_alternative<std::string>(resolved));
        EXPECT_FALSE(std::get<std::string>(resolved).empty());
      }
    });
  }

  resolve_barrier.wait();

  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(resolve_failures.load(), 0)
      << "Dangling data_refs detected: " << dangling_refs.size()
      << " refs could not be resolved";
}

TEST_F(DataBusConcurrentDataRefTest, ConcurrentStoreInterleavedWithRemove) {
  // Stress test: writers store_with_ref while a remover thread calls remove()
  // on the same keys. Invariant: resolve_ref must never return a garbled value
  // — either the value is intact or the ref simply cannot be resolved.
  static constexpr int kNumKeys = 5;
  static constexpr int kIterations = 30;

  ThreadBarrier barrier(3);  // 2 writers + 1 remover
  std::atomic<int> store_ok{0};
  std::atomic<int> resolve_ok{0};
  std::atomic<int> resolve_fail{0};
  std::vector<MMDataBus::DataRef> refs(kNumKeys);
  std::atomic<bool> stop{false};

  // Writer 1
  auto writer1 = std::thread([&]() {
    barrier.wait();
    for (int iter = 0; iter < kIterations && !stop; ++iter) {
      for (int k = 0; k < kNumKeys; ++k) {
        MMDataBus::OwnerContext owner;
        owner.session_id = "stress-session";
        std::string key = "stress_k" + std::to_string(k);
        if (databus_.store_with_ref(key, DataVariant(std::string("w1_") + std::to_string(iter)), owner, refs[k])) {
          store_ok.fetch_add(1);
        }
      }
      std::this_thread::yield();
    }
  });

  // Writer 2
  auto writer2 = std::thread([&]() {
    barrier.wait();
    for (int iter = 0; iter < kIterations && !stop; ++iter) {
      for (int k = 0; k < kNumKeys; ++k) {
        MMDataBus::OwnerContext owner;
        owner.session_id = "stress-session";
        std::string key = "stress_k" + std::to_string(k);
        if (databus_.store_with_ref(key, DataVariant(std::string("w2_") + std::to_string(iter)), owner, refs[k])) {
          store_ok.fetch_add(1);
        }
      }
      std::this_thread::yield();
    }
  });

  // Remover
  auto remover = std::thread([&]() {
    barrier.wait();
    for (int iter = 0; iter < kIterations && !stop; ++iter) {
      for (int k = 0; k < kNumKeys; ++k) {
        std::string key = "stress_k" + std::to_string(k);
        databus_.remove(key);
        std::this_thread::yield();
      }
    }
  });

  writer1.join();
  writer2.join();
  remover.join();

  // Post-stress: resolve refs — some may be gone (removed), but must not be garbled.
  for (int k = 0; k < kNumKeys; ++k) {
    DataVariant resolved;
    if (databus_.resolve_ref(refs[k].address, resolved)) {
      resolve_ok.fetch_add(1);
      ASSERT_TRUE(std::holds_alternative<std::string>(resolved));
      std::string val = std::get<std::string>(resolved);
      EXPECT_TRUE(val.rfind("w1_", 0) == 0 || val.rfind("w2_", 0) == 0)
          << "Garbled data_ref value: " << val;
    } else {
      resolve_fail.fetch_add(1);
      // resolve fail is acceptable — the value may have been removed
    }
  }
}

// ============================================================================
// Test 3: Session 隔离边界
//
// Two NodeDataBusClientImpl with different session IDs write same key name
// concurrently. Each session must only read back its own data.
// ============================================================================

class DataBusSessionIsolationTest : public ::testing::Test {
protected:
  MMDataBus databus_{1024 * 1024};
  NodeDataBusAdapter adapter_{&databus_};
};

TEST_F(DataBusSessionIsolationTest, ConcurrentWritesDontLeakBetweenSessions) {
  NodeDataBusClientImpl client_a(&databus_, "session-A");
  NodeDataBusClientImpl client_b(&databus_, "session-B");

  static constexpr int kNumKeys = 20;
  ThreadBarrier barrier(3);  // writer_a + writer_b + verifier

  std::atomic<int> failures{0};

  auto writer_a = std::thread([&]() {
    barrier.wait();
    for (int i = 0; i < kNumKeys; ++i) {
      std::string key = "session_key_" + std::to_string(i);
      if (!client_a.user()->put(key, SoDataVariant(std::string("from_A")))) {
        failures.fetch_add(1);
      }
    }
  });

  auto writer_b = std::thread([&]() {
    barrier.wait();
    for (int i = 0; i < kNumKeys; ++i) {
      std::string key = "session_key_" + std::to_string(i);
      if (!client_b.user()->put(key, SoDataVariant(std::string("from_B")))) {
        failures.fetch_add(1);
      }
    }
  });

  barrier.wait();

  writer_a.join();
  writer_b.join();

  ASSERT_EQ(failures.load(), 0);

  // Session A must see only "from_A"
  for (int i = 0; i < kNumKeys; ++i) {
    std::string key = "session_key_" + std::to_string(i);
    SoDataVariant val;
    ASSERT_TRUE(client_a.user()->get(key, val)) << "Session-A missing key: " << key;
    ASSERT_TRUE(std::holds_alternative<std::string>(val));
    EXPECT_EQ(std::get<std::string>(val), "from_A")
        << "Session-A sees leaked data at key: " << key;
  }

  // Session B must see only "from_B"
  for (int i = 0; i < kNumKeys; ++i) {
    std::string key = "session_key_" + std::to_string(i);
    SoDataVariant val;
    ASSERT_TRUE(client_b.user()->get(key, val)) << "Session-B missing key: " << key;
    ASSERT_TRUE(std::holds_alternative<std::string>(val));
    EXPECT_EQ(std::get<std::string>(val), "from_B")
        << "Session-B sees leaked data at key: " << key;
  }

  // Negative check: session must NOT contain the other's key prefix
  std::string other_key_a = "session_key_0";
  SoDataVariant should_not_exist;
  EXPECT_FALSE(client_a.sys()->contains(other_key_a))
      << "Session-A should not see sys namespace keys from concurrent test";
}

TEST_F(DataBusSessionIsolationTest, SessionNamespacePrefixesDoNotCollide) {
  // Verify the actual key prefix is /session/{id}/user/{key} for each session.
  // Direct MMDataBus read should find both keys with different full paths.
  NodeDataBusClientImpl client_a(&databus_, "session-A");
  NodeDataBusClientImpl client_b(&databus_, "session-B");

  client_a.user()->put("shared_name", SoDataVariant(std::string("A_value")));
  client_b.user()->put("shared_name", SoDataVariant(std::string("B_value")));

  // Read via raw (unprefixed) DataBus using the expected full paths
  SoDataVariant raw_a, raw_b;
  EXPECT_TRUE(adapter_.get("/session/session-A/user/shared_name", raw_a));
  EXPECT_TRUE(adapter_.get("/session/session-B/user/shared_name", raw_b));

  ASSERT_TRUE(std::holds_alternative<std::string>(raw_a));
  ASSERT_TRUE(std::holds_alternative<std::string>(raw_b));
  EXPECT_EQ(std::get<std::string>(raw_a), "A_value");
  EXPECT_EQ(std::get<std::string>(raw_b), "B_value");
}

TEST_F(DataBusSessionIsolationTest, ConcurrentReadWriteDifferentNamespaces) {
  // Multiple threads write to different namespaces (user, llm, sys) for the
  // same session. Must not cross-contaminate.
  NodeDataBusClientImpl client(&databus_, "session-ns-test");

  static constexpr int kNumThreads = 3;
  static constexpr int kItemsPerThread = 30;
  ThreadBarrier barrier(kNumThreads + 1);
  std::atomic<int> failures{0};

  auto writer_user = std::thread([&]() {
    barrier.wait();
    for (int i = 0; i < kItemsPerThread; ++i) {
      if (!client.user()->put("ns_k" + std::to_string(i), SoDataVariant(std::string("user")))) {
        failures.fetch_add(1);
      }
    }
  });

  auto writer_llm = std::thread([&]() {
    barrier.wait();
    for (int i = 0; i < kItemsPerThread; ++i) {
      if (!client.llm()->put("ns_k" + std::to_string(i), SoDataVariant(std::string("llm")))) {
        failures.fetch_add(1);
      }
    }
  });

  auto writer_sys = std::thread([&]() {
    barrier.wait();
    for (int i = 0; i < kItemsPerThread; ++i) {
      if (!client.sys()->put("ns_k" + std::to_string(i), SoDataVariant(std::string("sys")))) {
        failures.fetch_add(1);
      }
    }
  });

  barrier.wait();

  writer_user.join();
  writer_llm.join();
  writer_sys.join();

  ASSERT_EQ(failures.load(), 0);

  // Verify isolation
  for (int i = 0; i < kItemsPerThread; ++i) {
    std::string key = "ns_k" + std::to_string(i);

    SoDataVariant user_val;
    ASSERT_TRUE(client.user()->get(key, user_val));
    EXPECT_EQ(std::get<std::string>(user_val), "user");

    SoDataVariant llm_val;
    ASSERT_TRUE(client.llm()->get(key, llm_val));
    EXPECT_EQ(std::get<std::string>(llm_val), "llm");

    SoDataVariant sys_val;
    ASSERT_TRUE(client.sys()->get(key, sys_val));
    EXPECT_EQ(std::get<std::string>(sys_val), "sys");
  }
}

}  // namespace
}  // namespace runtime
}  // namespace agentflow
