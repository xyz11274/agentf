// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating proper use of actor adapter
// This shows how to write AgentFlow code that is independent of Protoactor

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "vendor/actor/adapter.h"
#include "utils/logger.h"
#include "agent/common.h"

using namespace agentflow;
using namespace agentflow::actor::adapter;

// Example 1: Simple actor using adapter interface
class EchoActor : public IActor {
private:
  std::string actor_id_;

public:
  explicit EchoActor(const std::string& id) : actor_id_(id) {}

  void on_message(std::shared_ptr<IContext> ctx) override {
    auto msg = ctx->current_message();

    // Log message reception
    Logger& logger = Logger::instance();
    logger.log(Logger::Level::INFO, "EchoActor [" + actor_id_ + "] received: " + msg.message_type);

    // Echo back to sender if there is one
    auto sender = ctx->sender();
    if(sender) {
      ctx->send(sender.get(), msg);
    }
  }

  void on_start() override {
    Logger& logger = Logger::instance();
    logger.log(Logger::Level::INFO, "EchoActor [" + actor_id_ + "] started");
  }

  void on_stop() override {
    Logger& logger = Logger::instance();
    logger.log(Logger::Level::INFO, "EchoActor [" + actor_id_ + "] stopping");
  }

  std::string actor_type() const override { return "EchoActor"; }
};

// Example 2: Counter actor
class CounterActor : public IActor {
private:
  int count_ = 0;

  struct CountMessage {
    int value;
  };

  struct GetCountMessage {
    // Empty - just a signal
  };

public:
  void on_message(std::shared_ptr<IContext> ctx) override {
    auto msg = ctx->current_message();

    if(msg.message_type == "increment") {
      count_++;
      Logger& logger = Logger::instance();
      logger.log(Logger::Level::INFO, "Counter incremented: " + std::to_string(count_));
    } else if(msg.message_type == "decrement") {
      count_--;
      Logger& logger = Logger::instance();
      logger.log(Logger::Level::INFO, "Counter decremented: " + std::to_string(count_));
    }
  }

  std::string actor_type() const override { return "CounterActor"; }
};

// Example 3: Actor factory
class ExampleActorFactory {
public:
  static std::shared_ptr<IActor> create_echo_actor(const std::string& id) {
    return std::make_shared<EchoActor>(id);
  }

  static std::shared_ptr<IActor> create_counter_actor() { return std::make_shared<CounterActor>(); }
};

int main() {
  std::cout << "=== AgentFlow Adapter Example ===\n\n";

  Logger& logger = Logger::instance();
  logger.log(Logger::Level::INFO, "Starting adapter example");
  logger.log(Logger::Level::INFO,
             "Actor implementation: " + ActorSystemFactory::implementation_name());

  // Create actor system
  auto system = ActorSystemFactory::create("example-system");
  if(!system) {
    std::cerr << "Failed to create actor system\n";
    return 1;
  }

  logger.log(Logger::Level::INFO, "Actor system created: " + system->system_address());

  // Spawn echo actors
  std::cout << "\n--- Spawning Actors ---\n";

  auto echo_factory = []() -> std::shared_ptr<IActor> {
    return ExampleActorFactory::create_echo_actor("echo-1");
  };

  auto echo_ref = system->spawn(echo_factory, "echo-actor-1");
  if(!echo_ref) {
    logger.log(Logger::Level::ERROR, "Failed to spawn echo actor");
    return 1;
  }

  logger.log(Logger::Level::INFO, "Echo actor spawned: " + echo_ref->to_string());

  // Spawn counter actor
  auto counter_factory = []() -> std::shared_ptr<IActor> {
    return ExampleActorFactory::create_counter_actor();
  };

  auto counter_ref = system->spawn(counter_factory, "counter-actor");
  if(!counter_ref) {
    logger.log(Logger::Level::ERROR, "Failed to spawn counter actor");
    return 1;
  }

  logger.log(Logger::Level::INFO, "Counter actor spawned: " + counter_ref->to_string());

  // Send messages
  std::cout << "\n--- Sending Messages ---\n";

  // Create messages using the builder utility
  auto msg1 = MessageBuilder::create("hello", std::string("test message"));
  logger.log(Logger::Level::INFO, "Created message: " + msg1.message_type);

  auto msg2 = MessageBuilder::create<int>("number", 42);
  logger.log(Logger::Level::INFO, "Created numeric message");

  // In a real scenario, you would send these messages
  // This example just demonstrates the API

  std::cout << "\n--- Checking State ---\n";

  if(echo_ref->is_alive()) {
    logger.log(Logger::Level::INFO, "Echo actor is alive");
  }

  if(counter_ref->is_alive()) {
    logger.log(Logger::Level::INFO, "Counter actor is alive");
  }

  std::cout << "\n--- Shutting Down ---\n";

  // Stop actors
  system->stop(echo_ref);
  logger.log(Logger::Level::INFO, "Echo actor stopped");

  system->stop(counter_ref);
  logger.log(Logger::Level::INFO, "Counter actor stopped");

  // Shutdown system
  system->shutdown();
  logger.log(Logger::Level::INFO, "Actor system shutdown");

  std::cout << "\n=== Example Complete ===\n\n";

  return 0;
}
