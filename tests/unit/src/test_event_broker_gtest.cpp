// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include <thread>

#include "daemon/event_broker.h"
#include "daemon/rpc_server.h"

namespace agentflow {
namespace daemon {
namespace tests {

class EventBrokerTest : public ::testing::Test {
protected:
  void SetUp() override {
    broker_ = std::make_unique<EventBroker>();
  }

  void TearDown() override {
    broker_.reset();
  }

  std::unique_ptr<EventBroker> broker_;
};

TEST_F(EventBrokerTest, SubscribeReturnsPositiveId) {
  int sub_id = broker_->subscribe(1, {"task", "workflow"});
  EXPECT_GT(sub_id, 0);

  int sub_id2 = broker_->subscribe(2, {"agent"});
  EXPECT_GT(sub_id2, 0);
  EXPECT_NE(sub_id, sub_id2);
}

TEST_F(EventBrokerTest, UnsubscribeExisting) {
  int sub_id = broker_->subscribe(1, {"task"});
  EXPECT_TRUE(broker_->unsubscribe(sub_id));
}

TEST_F(EventBrokerTest, UnsubscribeNonexistent) {
  EXPECT_FALSE(broker_->unsubscribe(99999));
}

TEST_F(EventBrokerTest, UnsubscribeClient) {
  int sub1 = broker_->subscribe(1, {"task"});
  int sub2 = broker_->subscribe(1, {"workflow"});
  int sub3 = broker_->subscribe(2, {"agent"});

  broker_->unsubscribe_client(1);

  EXPECT_FALSE(broker_->unsubscribe(sub1));
  EXPECT_FALSE(broker_->unsubscribe(sub2));
  // Client 2's subscription should still exist
  EXPECT_TRUE(broker_->unsubscribe(sub3));
}

TEST_F(EventBrokerTest, EmitEventNoSubscribers) {
  // Should not crash
  broker_->emit_event("task", "{\"type\":\"start\"}");
}

}  // namespace tests
}  // namespace daemon
}  // namespace agentflow
