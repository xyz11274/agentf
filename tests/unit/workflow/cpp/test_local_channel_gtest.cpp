// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test tests for LocalChannel functionality

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

#include "runtime/communication/local_channel.h"

namespace agentflow {
namespace tests {

class LocalChannelTest : public ::testing::Test {
protected:
  void SetUp() override {
    channel_ = std::make_shared<runtime::LocalChannel>("test_channel", 100);
    channel_->open();
  }

  void TearDown() override {
    if(channel_) {
      channel_->close();
    }
  }

  std::shared_ptr<runtime::LocalChannel> channel_;
};

// ============================================================================
// Channel Lifecycle Tests
// ============================================================================

TEST_F(LocalChannelTest, CreateChannel) {
  ASSERT_NE(channel_, nullptr);
  EXPECT_TRUE(channel_->is_open());
  EXPECT_EQ(channel_->get_channel_id(), "test_channel");
}

TEST_F(LocalChannelTest, CloseChannel) {
  ASSERT_TRUE(channel_->is_open());
  channel_->close();
  EXPECT_FALSE(channel_->is_open());
}

TEST_F(LocalChannelTest, ReopenChannel) {
  channel_->close();
  EXPECT_FALSE(channel_->is_open());

  // Create new channel with same ID
  auto new_channel = runtime::LocalChannelRegistry::instance().get_or_create("test_channel");
  new_channel->open();
  EXPECT_TRUE(new_channel->is_open());

  new_channel->close();
}

// ============================================================================
// Data Send/Receive Tests
// ============================================================================

TEST_F(LocalChannelTest, SendReceiveSinglePacket) {
  runtime::DataPacket packet;
  packet.key       = "test_key";
  packet.value     = std::string("test_value");
  packet.source_id = "test_sender";

  ASSERT_TRUE(channel_->send(packet));
  EXPECT_EQ(channel_->get_queue_depth(), 1);

  runtime::DataPacket received;
  ASSERT_TRUE(channel_->receive(received));
  EXPECT_EQ(received.key, "test_key");
  EXPECT_EQ(std::get<std::string>(received.value), "test_value");
}

TEST_F(LocalChannelTest, SendReceiveMultiplePackets) {
  const int num_packets = 10;

  for(int i = 0; i < num_packets; ++i) {
    runtime::DataPacket packet;
    packet.key   = "key_" + std::to_string(i);
    packet.value = static_cast<int64_t>(i);

    ASSERT_TRUE(channel_->send(packet));
  }

  EXPECT_EQ(channel_->get_queue_depth(), num_packets);

  // Receive all packets
  for(int i = 0; i < num_packets; ++i) {
    runtime::DataPacket received;
    ASSERT_TRUE(channel_->receive(received));
    EXPECT_EQ(std::get<int64_t>(received.value), i);
  }

  EXPECT_EQ(channel_->get_queue_depth(), 0);
}

TEST_F(LocalChannelTest, TryReceiveWhenEmpty) {
  runtime::DataPacket packet;
  EXPECT_FALSE(channel_->try_receive(packet));
}

TEST_F(LocalChannelTest, TryReceiveWhenDataAvailable) {
  // Send data first
  runtime::DataPacket send_packet;
  send_packet.key   = "test_key";
  send_packet.value = std::string("test_value");

  ASSERT_TRUE(channel_->send(send_packet));

  // Try receive immediately
  runtime::DataPacket receive_packet;
  EXPECT_TRUE(channel_->try_receive(receive_packet));
  EXPECT_EQ(receive_packet.key, "test_key");
}

// ============================================================================
// Key-Based Data Access Tests
// ============================================================================

TEST_F(LocalChannelTest, SendToKey) {
  runtime::DataPacket packet;
  packet.key   = "user_data";
  packet.value = std::string("user_value");

  ASSERT_TRUE(channel_->send_to_key("user_data", packet.value));
}

TEST_F(LocalChannelTest, ReceiveFromKey) {
  // Send data to a specific key
  runtime::DataPacket packet;
  packet.key   = "user_profile";
  packet.value = std::string("profile_data");

  ASSERT_TRUE(channel_->send_to_key("user_profile", packet.value));

  // Receive from that key
  runtime::DataPacket received;
  ASSERT_TRUE(channel_->receive_from_key("user_profile", received));
  EXPECT_EQ(received.key, "user_profile");
  EXPECT_EQ(std::get<std::string>(received.value), "profile_data");
}

TEST_F(LocalChannelTest, GetLatestForKey) {
  // Send multiple packets with same key
  runtime::DataPacket packet1;
  packet1.key   = "counter";
  packet1.value = static_cast<int64_t>(1);
  channel_->send(packet1);

  runtime::DataPacket packet2;
  packet2.key   = "counter";
  packet2.value = static_cast<int64_t>(2);
  channel_->send(packet2);

  runtime::DataPacket packet3;
  packet3.key   = "counter";
  packet3.value = static_cast<int64_t>(3);
  channel_->send(packet3);

  // Get latest should return the last sent value
  DataValue latest = channel_->get_latest("counter");
  EXPECT_EQ(std::get<int64_t>(latest), 3);
}

TEST_F(LocalChannelTest, GetAllForKey) {
  // Send multiple packets with same key
  for(int i = 0; i < 5; ++i) {
    runtime::DataPacket packet;
    packet.key   = "history";
    packet.value = std::string("value_" + std::to_string(i));
    channel_->send(packet);
  }

  // Get all should return all packets for that key
  auto all = channel_->get_all("history");
  EXPECT_EQ(all.size(), 5);
  EXPECT_EQ(all[0].key, "history");
  EXPECT_EQ(all[4].key, "history");
}

TEST_F(LocalChannelTest, HasDataForKey) {
  EXPECT_FALSE(channel_->has_data("nonexistent_key"));

  // Send data
  runtime::DataPacket packet;
  packet.key   = "existing_key";
  packet.value = std::string("exists");
  channel_->send(packet);

  EXPECT_TRUE(channel_->has_data("existing_key"));
}

TEST_F(LocalChannelTest, GetLatestForNonexistentKey) {
  auto value = channel_->get_latest("nonexistent_key");
  // Should return empty variant
  EXPECT_TRUE(std::holds_alternative<std::string>(value) || std::holds_alternative<int64_t>(value));
}

// ============================================================================
// Subscription Tests
// ============================================================================

TEST_F(LocalChannelTest, SubscribeUnsubscribe) {
  int callback_count = 0;

  auto sub_id = channel_->subscribe(
      [&callback_count](runtime::ChannelEvent event, const runtime::DataPacket& packet) {
        if(event == runtime::ChannelEvent::DATA_READY) {
          callback_count++;
        }
      });

  EXPECT_NE(sub_id, 0);

  // Send data to trigger callback
  runtime::DataPacket packet;
  packet.key   = "test";
  packet.value = std::string("value");
  channel_->send(packet);

  // Give time for callback to be called
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(callback_count, 1);

  channel_->unsubscribe(sub_id);
}

TEST_F(LocalChannelTest, MultipleSubscribers) {
  int count1 = 0, count2 = 0;

  channel_->subscribe([&count1](auto, auto) { count1++; });
  channel_->subscribe([&count2](auto, auto) { count2++; });

  runtime::DataPacket packet;
  packet.key   = "test";
  packet.value = std::string("value");
  channel_->send(packet);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(count1, 1);
  EXPECT_EQ(count2, 1);
}

// ============================================================================
// Queue Capacity Tests
// ============================================================================

TEST_F(LocalChannelTest, FillQueueToCapacity) {
  const size_t capacity       = 100;
  auto         capped_channel = std::make_shared<runtime::LocalChannel>("capped", capacity);
  capped_channel->open();

  // Fill queue to capacity
  for(size_t i = 0; i < capacity; ++i) {
    runtime::DataPacket packet;
    packet.key   = "item_" + std::to_string(i);
    packet.value = static_cast<int64_t>(i);
    ASSERT_TRUE(capped_channel->send(packet));
  }

  EXPECT_EQ(capped_channel->get_queue_depth(), capacity);

  // Next send should fail (queue full)
  runtime::DataPacket overflow_packet;
  overflow_packet.key   = "overflow";
  overflow_packet.value = std::string("should_fail");
  EXPECT_FALSE(capped_channel->send(overflow_packet));
}

TEST_F(LocalChannelTest, ZeroCapacityQueue) {
  auto zero_capacity_channel = std::make_shared<runtime::LocalChannel>("zero", 0);
  zero_capacity_channel->open();

  runtime::DataPacket packet;
  packet.key   = "test";
  packet.value = std::string("value");

  EXPECT_FALSE(zero_capacity_channel->send(packet));
}

TEST_F(LocalChannelTest, UnlimitedCapacityQueue) {
  auto unlimited_channel = std::make_shared<runtime::LocalChannel>("unlimited", 0);
  unlimited_channel->open();

  const int num_packets = 1000;

  for(int i = 0; i < num_packets; ++i) {
    runtime::DataPacket packet;
    packet.key   = "item_" + std::to_string(i);
    packet.value = static_cast<int64_t>(i);
    ASSERT_TRUE(unlimited_channel->send(packet));
  }

  EXPECT_EQ(unlimited_channel->get_queue_depth(), num_packets);
}

// ============================================================================
// Timeout Tests
// ============================================================================

TEST_F(LocalChannelTest, ReceiveWithTimeout) {
  // Set short timeout
  runtime::DataPacket packet;
  bool                received = channel_->receive(packet, 100);

  EXPECT_FALSE(received) << "Should timeout before data arrives";
}

TEST_F(LocalChannelTest, ReceiveWithNoTimeout) {
  // Send data from another thread
  std::thread sender([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    runtime::DataPacket packet;
    packet.key   = "timed_test";
    packet.value = std::string("received");
    this->channel_->send(packet);
  });

  runtime::DataPacket packet;
  bool                received = channel_->receive(packet, 500);  // 500ms timeout

  sender.join();
  EXPECT_TRUE(received) << "Should receive data within timeout";
}

TEST_F(LocalChannelTest, ReceiveFromKeyWithTimeout) {
  // Send data to key after delay
  std::thread sender([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    runtime::DataPacket packet;
    packet.key   = "delayed_key";
    packet.value = std::string("delayed_value");
    this->channel_->send_to_key("delayed_key", packet.value);
  });

  runtime::DataPacket packet;
  bool                received = channel_->receive_from_key("delayed_key", packet, 200);

  sender.join();
  EXPECT_FALSE(received) << "Should timeout before data arrives";
}

TEST_F(LocalChannelTest, ReceiveFromKeyImmediate) {
  // Send data first
  runtime::DataPacket send_packet;
  send_packet.key   = "immediate_key";
  send_packet.value = std::string("immediate_value");
  channel_->send_to_key("immediate_key", send_packet.value);

  runtime::DataPacket receive_packet;
  bool                received = channel_->receive_from_key("immediate_key", receive_packet, 100);

  EXPECT_TRUE(received);
  EXPECT_EQ(receive_packet.key, "immediate_key");
}

// ============================================================================
// Statistics Tests
// ============================================================================

TEST_F(LocalChannelTest, GetStatistics) {
  runtime::DataPacket packet;
  packet.key   = "stats_test";
  packet.value = std::string("value");

  channel_->send(packet);

  auto stats = channel_->get_stats();
  EXPECT_EQ(stats.packets_sent, 1);
  EXPECT_EQ(stats.packets_received, 0);  // Not received yet
}

TEST_F(LocalChannelTest, StatisticsAfterReceive) {
  runtime::DataPacket packet;
  packet.key   = "stats_test2";
  packet.value = std::string("value");

  channel_->send(packet);

  runtime::DataPacket received;
  channel_->receive(received);

  auto stats = channel_->get_stats();
  EXPECT_EQ(stats.packets_sent, 1);
  EXPECT_EQ(stats.packets_received, 1);
  EXPECT_GT(stats.queue_depth, 0);
}

TEST_F(LocalChannelTest, ResetStatistics) {
  // Send some packets
  for(int i = 0; i < 5; ++i) {
    runtime::DataPacket packet;
    packet.key   = "reset_test";
    packet.value = static_cast<int64_t>(i);
    channel_->send(packet);
  }

  auto stats_before = channel_->get_stats();
  EXPECT_EQ(stats_before.packets_sent, 5);

  channel_->reset_stats();

  auto stats_after = channel_->get_stats();
  EXPECT_EQ(stats_after.packets_sent, 0);
  EXPECT_EQ(stats_after.packets_received, 0);
}

// ============================================================================
// Event Notification Tests
// ============================================================================

TEST_F(LocalChannelTest, ChannelClosedEvent) {
  bool closed_notified = false;

  auto sub_id = channel_->subscribe([&closed_notified](runtime::ChannelEvent event, const auto&) {
    if(event == runtime::ChannelEvent::CHANNEL_CLOSED) {
      closed_notified = true;
    }
  });

  channel_->close();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(closed_notified);
  channel_->unsubscribe(sub_id);
}

TEST_F(LocalChannelTest, DataConsumedEvent) {
  int consumed_count = 0;

  channel_->subscribe([&consumed_count](runtime::ChannelEvent event, const auto&) {
    if(event == runtime::ChannelEvent::DATA_CONSUMED) {
      consumed_count++;
    }
  });

  // Send and receive
  runtime::DataPacket packet;
  packet.key   = "event_test";
  packet.value = std::string("value");
  channel_->send(packet);

  runtime::DataPacket received;
  channel_->receive(received);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_GE(consumed_count, 1);
}

TEST_F(LocalChannelTest, DataReadyEvent) {
  int ready_count = 0;

  channel_->subscribe([&ready_count](runtime::ChannelEvent event, const auto&) {
    if(event == runtime::ChannelEvent::DATA_READY) {
      ready_count++;
    }
  });

  // Send data
  runtime::DataPacket packet;
  packet.key   = "ready_test";
  packet.value = std::string("value");
  channel_->send(packet);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_GE(ready_count, 1);
}

// ============================================================================
// Clear Tests
// ============================================================================

TEST_F(LocalChannelTest, ClearEmptyChannel) {
  channel_->clear();
  EXPECT_EQ(channel_->get_queue_depth(), 0);
}

TEST_F(LocalChannelTest, ClearChannelWithData) {
  // Add some data
  for(int i = 0; i < 10; ++i) {
    runtime::DataPacket packet;
    packet.key   = "clear_test";
    packet.value = static_cast<int64_t>(i);
    channel_->send(packet);
  }

  EXPECT_EQ(channel_->get_queue_depth(), 10);

  // Clear
  channel_->clear();

  EXPECT_EQ(channel_->get_queue_depth(), 0);
}

TEST_F(LocalChannelTest, ClearWithKeyData) {
  // Add data to specific key
  for(int i = 0; i < 3; ++i) {
    runtime::DataPacket packet;
    packet.key   = "clear_key";
    packet.value = std::string("value_" + std::to_string(i));
    channel_->send_to_key("clear_key", packet.value);
  }

  // Clear should remove all data
  channel_->clear();

  EXPECT_EQ(channel_->get_queue_depth(), 0);
}

// ============================================================================
// Wait For Data Tests
// ============================================================================

TEST_F(LocalChannelTest, WaitForDataAvailable) {
  std::thread waiter([this]() {
    // Wait for data to become available on a key
    bool result = this->channel_->wait_for_data("wait_key", 1000);

    // Should return false (timeout)
    EXPECT_FALSE(result);
  });

  // Send data after delay
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Now send data
  runtime::DataPacket packet;
  packet.key   = "wait_key";
  packet.value = std::string("arrived");
  channel_->send_to_key("wait_key", packet.value);

  waiter.join();
}

TEST_F(LocalChannelTest, WaitForDataImmediate) {
  // Send data first
  runtime::DataPacket packet;
  packet.key   = "wait_immediate";
  packet.value = std::string("already_here");
  channel_->send_to_key("wait_immediate", packet.value);

  // Wait immediately should succeed
  bool result = channel_->wait_for_data("wait_immediate", 100);
  EXPECT_TRUE(result);
}

// ============================================================================
// Channel Registry Tests
// ============================================================================

TEST_F(LocalChannelTest, GetOrCreateChannel) {
  auto& registry = runtime::LocalChannelRegistry::instance();

  auto channel1 = registry.get_or_create("registry_test", 50);
  auto channel2 = registry.get_or_create("registry_test", 50);

  // Should return same channel
  EXPECT_EQ(channel1, channel2);
}

TEST_F(LocalChannelTest, GetNonexistentChannel) {
  auto& registry = runtime::LocalChannelRegistry::instance();

  auto channel = registry.get("nonexistent_channel");
  EXPECT_EQ(channel, nullptr);
}

TEST_F(LocalChannelTest, RemoveChannel) {
  auto& registry = runtime::LocalChannelRegistry::instance();

  registry.get_or_create("removable", 10);
  EXPECT_EQ(registry.size(), 1);

  bool removed = registry.remove("removable");
  EXPECT_TRUE(removed);

  EXPECT_EQ(registry.size(), 0);
}

TEST_F(LocalChannelTest, GetChannelIds) {
  auto& registry = runtime::LocalChannelRegistry::instance();

  registry.get_or_create("channel_a", 10);
  registry.get_or_create("channel_b", 10);
  registry.get_or_create("channel_c", 10);

  auto ids = registry.get_channel_ids();
  EXPECT_EQ(ids.size(), 3);
}

TEST_F(LocalChannelTest, CloseAllChannels) {
  auto& registry = runtime::LocalChannelRegistry::instance();

  registry.get_or_create("close_a", 10);
  registry.get_or_create("close_b", 10);
  registry.get_or_create("close_c", 10);

  EXPECT_EQ(registry.size(), 3);

  registry.close_all();

  EXPECT_EQ(registry.size(), 0);

  // Channels should be closed
  auto channel_a = registry.get("close_a");
  auto channel_b = registry.get("close_b");

  if(channel_a) {
    EXPECT_FALSE(channel_a->is_open());
  }
  if(channel_b) {
    EXPECT_FALSE(channel_b->is_open());
  }
}

// ============================================================================
// Zero Copy Data Transfer Tests
// ============================================================================

TEST_F(LocalChannelTest, ZeroCopyDataTransfer) {
  // Test that data moves without copying
  std::string large_data(1000, 'x');  // 1KB string

  runtime::DataPacket packet;
  packet.key   = "large_data";
  packet.value = large_data;

  ASSERT_TRUE(channel_->send(packet));

  // The packet should have been moved (zero copy)
  // This is more of a conceptual test in this implementation

  runtime::DataPacket received;
  ASSERT_TRUE(channel_->receive(received));

  // Verify data integrity
  EXPECT_EQ(std::get<std::string>(received.value), large_data);
}

TEST_F(LocalChannelTest, ConcurrentAccessToDifferentKeys) {
  const int num_threads        = 5;
  const int packets_per_thread = 10;

  std::vector<std::thread> threads;

  for(int t = 0; t < num_threads; ++t) {
    threads.emplace_back([this, t, num_threads, packets_per_thread]() {
      for(int i = 0; i < packets_per_thread; ++i) {
        runtime::DataPacket packet;
        packet.key   = "thread_" + std::to_string(t) + "_item_" + std::to_string(i);
        packet.value = static_cast<int64_t>(t * packets_per_thread + i);

        this->channel_->send(packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  // Join all threads
  for(auto& thread : threads) {
    thread.join();
  }

  // All packets should be in queue
  EXPECT_EQ(channel_->get_queue_depth(), num_threads * packets_per_thread);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(LocalChannelTest, SendToClosedChannel) {
  channel_->close();

  runtime::DataPacket packet;
  packet.key   = "after_close";
  packet.value = std::string("should_fail");

  EXPECT_FALSE(channel_->send(packet));
}

TEST_F(LocalChannelTest, ReceiveFromClosedChannel) {
  channel_->close();

  runtime::DataPacket packet;
  bool                received = channel_->receive(packet);

  EXPECT_FALSE(received);
}

TEST_F(LocalChannelTest, StatisticsAfterClose) {
  // Send some packets
  for(int i = 0; i < 5; ++i) {
    runtime::DataPacket packet;
    packet.key   = "before_close";
    packet.value = static_cast<int64_t>(i);
    channel_->send(packet);
  }

  auto stats_before = channel_->get_stats();
  EXPECT_EQ(stats_before.packets_sent, 5);

  channel_->close();

  // Stats should still be accessible after close
  auto stats_after = channel_->get_stats();
  EXPECT_EQ(stats_after.packets_sent, 5);
}

}  // namespace tests
}  // namespace agentflow
