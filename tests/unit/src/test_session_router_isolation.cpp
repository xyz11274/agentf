#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>
#include "agent/session.h"
#include "agent/session_router.h"

using agentflow::framework::MMAgentSession;
using agentflow::framework::SessionRouter;
using agentflow::framework::MMAgentMsgQue;

namespace {
std::shared_ptr<MMAgentSession> make_session(const std::string& id = "") {
  MMAgentSession::Config config;
  config.session_id = id;
  return std::make_shared<MMAgentSession>(config);
}
}

TEST(SessionRouterIsolation, ConcurrentRoutesNeverShareAPhysicalSession) {
  for(int round = 0; round < 30; ++round) {
    SessionRouter router(SessionRouter::Config{});
    std::vector<std::shared_ptr<MMAgentSession>> pool;
    for(int i = 0; i < 12; ++i) pool.push_back(make_session("pool-" + std::to_string(i)));
    router.set_session_pool(pool);
    std::promise<void> start;
    auto gate = start.get_future().share();
    std::vector<std::future<std::shared_ptr<MMAgentSession>>> calls;
    for(int i = 0; i < 12; ++i) {
      calls.push_back(std::async(std::launch::async, [&, i] {
        gate.wait();
        return router.get_or_create_session("user-" + std::to_string(i), "client");
      }));
    }
    start.set_value();
    std::set<std::string> ids;
    for(int i = 0; i < 12; ++i) {
      auto session = calls[i].get();
      ASSERT_NE(session, nullptr);
      EXPECT_EQ(session->get_user_id(), "user-" + std::to_string(i));
      EXPECT_TRUE(ids.insert(session->get_session_id()).second);
    }
    EXPECT_EQ(router.get_active_route_keys().size(), 12u);
  }
}

TEST(SessionRouterIsolation, ConcurrentSameRouteInitializesExactlyOnce) {
  SessionRouter router(SessionRouter::Config{});
  std::atomic<int> created{0};
  router.set_session_factory([&](const auto&, const auto&) {
    ++created;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return make_session();
  });
  std::promise<void> start;
  auto gate = start.get_future().share();
  std::vector<std::future<std::shared_ptr<MMAgentSession>>> calls;
  for(int i = 0; i < 12; ++i) calls.push_back(std::async(std::launch::async, [&] {
    gate.wait(); return router.get_or_create_session("same-user", "same-client");
  }));
  start.set_value();
  auto first = calls[0].get();
  ASSERT_NE(first, nullptr);
  for(size_t i = 1; i < calls.size(); ++i) EXPECT_EQ(calls[i].get(), first);
  EXPECT_EQ(created.load(), 1);
}

TEST(SessionRouterIsolation, PhysicalHintCannotReplaceAnotherUsersIdentity) {
  SessionRouter router(SessionRouter::Config{});
  router.set_session_pool({make_session("physical")});
  auto owner = router.get_or_create_session("owner", "client-a");
  int delivered = 0;
  router.set_route_handler([&](const auto&, const auto&) { ++delivered; });
  MMAgentMsgQue::Message message;
  message.metadata = {{"user_id", "other"}, {"client_id", "client-b"}, {"session_id", "physical"}};
  EXPECT_FALSE(router.route_request_message(message));
  EXPECT_EQ(delivered, 0);
  EXPECT_EQ(owner->get_user_id(), "owner");
  EXPECT_EQ(router.get_route_key_for_session("physical"), "owner:client-a");
}

TEST(SessionRouterIsolation, FactoryFailureReleasesPendingRoute) {
  SessionRouter router(SessionRouter::Config{});
  std::atomic<int> attempts{0};
  router.set_session_factory([&](const auto&, const auto&) {
    if(++attempts == 1) throw std::runtime_error("test construction failure");
    return make_session();
  });
  EXPECT_THROW(router.get_or_create_session("user", "client"), std::runtime_error);
  auto second = router.get_or_create_session("user", "client");
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->get_user_id(), "user");
  EXPECT_EQ(attempts.load(), 2);
}

TEST(SessionRouterIsolation, BindCallbackMayReadRouterWithoutDeadlocking) {
  SessionRouter router(SessionRouter::Config{});
  std::atomic<int> callbacks{0};
  router.set_session_factory([&](const auto&, const auto&) {
    auto session = make_session();
    session->set_session_event_callback([&](const auto&, const auto&, const auto&, auto) {
      (void)router.get_active_route_keys();
      ++callbacks;
    });
    return session;
  });
  ASSERT_NE(router.get_or_create_session("user", "client"), nullptr);
  EXPECT_EQ(callbacks.load(), 1);
}
