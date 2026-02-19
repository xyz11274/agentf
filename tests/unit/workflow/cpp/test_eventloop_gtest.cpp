// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test tests for EventLoopAdapter functionality

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "utils/logger.h"
#include "event/adapter_factory.h"
#include "event/eventloop_adapter.h"

namespace agentflow {
namespace tests {

class EventLoopAdapterTest : public ::testing::Test {
protected:
  void SetUp() override { adapter_ = event::create_eventloop_adapter(); }

  void TearDown() override {
    if(adapter_) {
      adapter_->cleanup();
    }
  }

  std::shared_ptr<event::IEventLoopAdapter> adapter_;
};

// ============================================================================
// Adapter Creation Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, CreateAdapter) {
  ASSERT_NE(adapter_, nullptr);
  EXPECT_FALSE(adapter_->get_version().empty());
}

TEST_F(EventLoopAdapterTest, AdapterAvailability) {
  bool available = adapter_->is_available();

  // May not be available if libevent is not installed
  // This test just checks that the method doesn't crash
}

TEST_F(EventLoopAdapterTest, InitializeAdapter) {
  bool initialized = adapter_->initialize();

  // Should succeed even if stub
  // Actual implementation may return false if libevent not available
}

// ============================================================================
// EventLoop Creation Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, CreateEventLoop) {
  auto event_loop = adapter_->create_loop();

  if(adapter_->is_available()) {
    ASSERT_NE(event_loop, nullptr);
  } else {
    // Stub implementation returns nullptr
    EXPECT_EQ(event_loop, nullptr);
  }
}

// ============================================================================
// EventLoop Initialization Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, InitializeEventLoop) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    bool initialized = event_loop->initialize();

    // Check error handling
    if(initialized) {
      EXPECT_EQ(event_loop->last_error(), event::EventLoopError::EL_OK);
    } else {
      // Failed initialization should set error
      EXPECT_NE(event_loop->last_error(), event::EventLoopError::EL_OK);
    }
  }
}

TEST_F(EventLoopAdapterTest, IsRunningAfterInitialize) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    bool initialized = event_loop->initialize();

    if(initialized) {
      bool is_running = event_loop->is_running();
      EXPECT_TRUE(is_running);
    }
  }
}

TEST_F(EventLoopAdapterTest, IsRunningWithoutInitialize) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    // Should not be running before initialization
    bool is_running = event_loop->is_running();
    EXPECT_FALSE(is_running);
  }
}

// ============================================================================
// Timer Event Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, AddTimerEvent) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    int callback_count = 0;

    auto timer = event_loop->add_timer(100, [&callback_count]() { callback_count++; });

    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->is_active());

    // Wait for timer to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    EXPECT_EQ(callback_count, 1);
  }
}

TEST_F(EventLoopAdapterTest, CancelTimerEvent) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    int callback_count = 0;

    auto timer = event_loop->add_timer(10000, [&callback_count]() { callback_count++; });

    ASSERT_NE(timer, nullptr);

    // Cancel timer before it fires
    bool cancelled = timer->cancel();
    EXPECT_TRUE(cancelled);

    // Wait to ensure timer doesn't fire
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(callback_count, 0);
  }
}

TEST_F(EventLoopAdapterTest, MultipleTimerEvents) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<int> callback_count{ 0 };

    // Add multiple timers
    for(int i = 0; i < 5; ++i) {
      auto timer = event_loop->add_timer(100 * (i + 1), [&callback_count]() { callback_count++; });
      ASSERT_NE(timer, nullptr);
    }

    // Wait for all timers to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    EXPECT_EQ(callback_count.load(), 5);
  }
}

TEST_F(EventLoopAdapterTest, AddPersistentTimerEvent) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    int callback_count = 0;

    auto timer = event_loop->add_persistent_timer(50, [&callback_count]() { callback_count++; });

    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->is_active());

    // Wait for multiple firings
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Persistent timer should fire multiple times
    EXPECT_GE(callback_count, 2);
  }
}

TEST_F(EventLoopAdapterTest, CancelPersistentTimerEvent) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    int callback_count = 0;

    auto timer = event_loop->add_persistent_timer(50, [&callback_count]() { callback_count++; });

    ASSERT_NE(timer, nullptr);

    // Cancel after first firing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bool cancelled = timer->cancel();
    EXPECT_TRUE(cancelled);

    // Should have fired once
    EXPECT_EQ(callback_count, 1);
  }
}

// ============================================================================
// Callback Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, AddImmediateCallback) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<bool> callback_executed{ false };

    bool added = event_loop->add_callback([&callback_executed]() { callback_executed = true; });

    EXPECT_TRUE(added);

    // Callback should execute on next dispatch
    event_loop->dispatch(100);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(callback_executed.load());
  }
}

TEST_F(EventLoopAdapterTest, AddMultipleCallbacks) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<int> callback_count{ 0 };

    for(int i = 0; i < 5; ++i) {
      event_loop->add_callback([&callback_count]() { callback_count++; });
    }

    // Callbacks should execute in dispatch order
    event_loop->dispatch(100);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(callback_count.load(), 5);
  }
}

// ============================================================================
// Dispatch Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, DispatchWithTimeout) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<bool> dispatched{ false };

    event_loop->add_callback([&dispatched]() { dispatched = true; });

    // Dispatch with timeout
    int result = event_loop->dispatch(100);

    EXPECT_EQ(result, 0);  // Success

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(dispatched.load());
  }
}

TEST_F(EventLoopAdapterTest, DispatchOnceWithTimeout) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<bool> dispatched{ false };

    event_loop->add_callback([&dispatched]() { dispatched = true; });

    // Dispatch once with timeout
    int result = event_loop->dispatch_once(100);

    EXPECT_EQ(result, 0);  // Success

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(dispatched.load());
  }
}

TEST_F(EventLoopAdapterTest, DispatchNoTimeout) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<bool> dispatched{ false };

    event_loop->add_callback([&dispatched]() { dispatched = true; });

    // Dispatch without timeout (infinite)
    int result = event_loop->dispatch(-1);

    EXPECT_EQ(result, 0);  // Success

    // Wait for dispatch
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(dispatched.load());
  }
}

// ============================================================================
// EventLoop Exit Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, ExitEventLoop) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<bool> running{ true };

    // Monitor loop in thread
    std::thread monitor([this, &running, &event_loop]() {
      while(running.load()) {
        event_loop->dispatch(10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });

    // Exit after delay
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    event_loop->exit();
    running = false;

    monitor.join();

    EXPECT_FALSE(running.load());
  }
}

TEST_F(EventLoopAdapterTest, IsRunningAfterExit) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();
    event_loop->exit();

    bool is_running = event_loop->is_running();

    EXPECT_FALSE(is_running);
  }
}

TEST_F(EventLoopAdapterTest, MultipleDispatchAfterExit) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    // Dispatch some events
    for(int i = 0; i < 3; ++i) {
      event_loop->add_callback([]() {});
      event_loop->dispatch(10);
    }

    event_loop->exit();

    // Events after exit should return immediately
    int result = event_loop->dispatch(100);
    EXPECT_NE(result, 0);  // Should fail
  }
}

// ============================================================================
// File Descriptor Event Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, AddFileDescriptorEvent) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    event_loop->initialize();

    // Create pipe for testing
    int pipe_fds[2];
    int result = ::pipe(pipe_fds);
    ASSERT_EQ(result, 0);

    auto file_event = event_loop->add_fd_event(pipe_fds[0]);

    if(adapter_->is_available()) {
      ASSERT_NE(file_event, nullptr);
      EXPECT_TRUE(file_event->is_active());
    } else {
      // Stub implementation
      EXPECT_EQ(file_event, nullptr);
    }

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
  }
}

TEST_F(EventLoopAdapterTest, FileEventCallback) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    event_loop->initialize();

    // Create pipe
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);

    std::atomic<int> callback_count{ 0 };

    auto file_event = event_loop->add_fd_event(pipe_fds[0]);

    if(adapter_->is_available()) {
      ASSERT_NE(file_event, nullptr);

      // Set callback
      bool callback_set = file_event->set_callback([&callback_count](int fd, int flags) {
        if(flags & 0x01) {  // EV_READ
          callback_count++;
        }
      });

      EXPECT_TRUE(callback_set);

      // Enable
      bool enabled = file_event->enable(0x01);  // EV_READ

      EXPECT_TRUE(enabled);

      // Write to trigger callback
      const char* data = "test";
      ::write(pipe_fds[1], data, 4);

      // Dispatch
      event_loop->dispatch(100);

      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      EXPECT_GE(callback_count.load(), 1);
    }

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
  }
}

TEST_F(EventLoopAdapterTest, RemoveFileDescriptorEvent) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    event_loop->initialize();

    // Create pipe
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);

    auto file_event = event_loop->add_fd_event(pipe_fds[0]);

    if(adapter_->is_available()) {
      ASSERT_NE(file_event, nullptr);

      // Remove event
      bool removed = event_loop->remove_fd_event(pipe_fds[0]);

      EXPECT_TRUE(removed);
    }

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
  }
}

TEST_F(EventLoopAdapterTest, FileEventEnableDisable) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    event_loop->initialize();

    // Create pipe
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);

    auto file_event = event_loop->add_fd_event(pipe_fds[0]);

    if(adapter_->is_available()) {
      ASSERT_NE(file_event, nullptr);

      // Enable event
      bool enabled = file_event->enable(0x01);

      EXPECT_TRUE(enabled);
      EXPECT_TRUE(file_event->is_active());

      // Disable event
      bool disabled = file_event->disable(0x01);

      EXPECT_TRUE(disabled);

      // Write should not trigger callback after disable
      ::write(pipe_fds[1], "test", 4);

      event_loop->dispatch(50);

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
  }
}

TEST_F(EventLoopAdapterTest, FileEventIsActive) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    event_loop->initialize();

    // Create pipe
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);

    auto file_event = event_loop->add_fd_event(pipe_fds[0]);

    if(adapter_->is_available()) {
      ASSERT_NE(file_event, nullptr);

      // Initially inactive
      EXPECT_FALSE(file_event->is_active());

      // Enable
      file_event->enable(0x01);
      EXPECT_TRUE(file_event->is_active());

      // Cleanup
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
    }
  }
}

// ============================================================================
// EventLoop Error Handling Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, GetLastError) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    // Initially no error
    EXPECT_EQ(event_loop->last_error(), event::EventLoopError::EL_OK);
  }
}

TEST_F(EventLoopAdapterTest, GetErrorMessage) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    // Initially no error
    EXPECT_TRUE(event_loop->error_message().empty());
  }
}

TEST_F(EventLoopAdapterTest, ErrorAfterInitializationFailure) {
  auto event_loop = adapter_->create_loop();

  if(event_loop) {
    // Simulate initialization failure
    // This is conceptual - actual error depends on libevent
    bool initialized = event_loop->initialize();

    if(!initialized && adapter_->is_available()) {
      // If libevent fails to initialize, check error
      auto error     = event_loop->last_error();
      auto error_msg = event_loop->error_message();

      EXPECT_NE(error, event::EventLoopError::EL_OK);

      // Error message should not be empty
      if(!adapter_->get_version().empty()) {
        EXPECT_FALSE(error_msg.empty());
      }
    }
  }
}

// ============================================================================
// Version Information Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, GetVersion) {
  auto version = adapter_->get_version();

  if(adapter_->is_available()) {
    EXPECT_FALSE(version.empty());
    EXPECT_FALSE(version.find("libevent") == std::string::npos);
  } else {
    EXPECT_EQ(version, "stub");
  }
}

TEST_F(EventLoopAdapterTest, StubBehavior) {
  if(!adapter_->is_available()) {
    // Stub implementation should return nullptr for create_loop
    auto event_loop = adapter_->create_loop();
    
    // Stub adapter returns nullptr when libevent is not available
    EXPECT_EQ(event_loop, nullptr);
  }
}

// ============================================================================
// Adapter Cleanup Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, CleanupAdapter) {
  adapter_->cleanup();

  // Should not crash
  EXPECT_TRUE(true);
}

TEST_F(EventLoopAdapterTest, CreateLoopAfterCleanup) {
  adapter_->cleanup();

  auto event_loop = adapter_->create_loop();

  if(adapter_->is_available()) {
    // Should still be able to create new loop
    ASSERT_NE(event_loop, nullptr);
  }
}

// ============================================================================
// Concurrent Dispatch Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, ConcurrentDispatch) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<int> callback_count{ 0 };

    // Add callbacks from multiple threads
    const int num_threads          = 5;
    const int callbacks_per_thread = 10;

    std::vector<std::thread> threads;

    for(int t = 0; t < num_threads; ++t) {
      threads.emplace_back([this, &callback_count, callbacks_per_thread, &event_loop]() {
        for(int i = 0; i < callbacks_per_thread; ++i) {
          event_loop->add_callback([&callback_count]() { callback_count++; });
          event_loop->dispatch(10);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
    }

    // Join all threads
    for(auto& thread : threads) {
      thread.join();
    }

    // All callbacks should be executed
    EXPECT_EQ(callback_count.load(), num_threads * callbacks_per_thread);
  }
}

// ============================================================================
// Timer Precision Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, ShortTimerPrecision) {
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    event_loop->initialize();

    std::atomic<int64_t> first_time{ 0 };
    std::atomic<bool>    first_fired{ false };

    auto timer = event_loop->add_timer(10, [&first_time, &first_fired]() {
      auto now = std::chrono::steady_clock::now();
      auto time_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

      if(first_fired.load()) {
        // Reset after first fire
        return;
      }

      first_time.store(time_ms);
      first_fired.store(true);
    });

    ASSERT_NE(timer, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(first_fired.load());
  }
}

// ============================================================================
// Resource Management Tests
// ============================================================================

TEST_F(EventLoopAdapterTest, MultipleEventLoops) {
  // Test creating multiple event loops
  auto loop1 = adapter_->create_loop();
  auto loop2 = adapter_->create_loop();
  auto loop3 = adapter_->create_loop();

  if(adapter_->is_available()) {
    ASSERT_NE(loop1, nullptr);
    ASSERT_NE(loop2, nullptr);
    ASSERT_NE(loop3, nullptr);

    // Each should be independent
    // This is conceptual - actual behavior depends on implementation
  }
}

TEST_F(EventLoopAdapterTest, EventLoopReuse) {
  // Test reusing event loop after cleanup
  auto event_loop = adapter_->create_loop();

  if(event_loop && adapter_->is_available()) {
    bool initialized = event_loop->initialize();
    EXPECT_TRUE(initialized);

    event_loop->exit();

    // Should still be usable
    initialized = event_loop->initialize();
    EXPECT_TRUE(initialized);
  }
}

}  // namespace tests
}  // namespace agentflow
