#include "trajectory_evolution/evaluation_entry.h"
#include "../../clients/c/evolution.h"
#include "integration_json.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <openssl/rand.h>

extern "C" int te_evaluate(te_evaluation_receiver receiver, void* context) {
  const auto* enabled = std::getenv("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY");
  if(!enabled || std::strcmp(enabled, "1")) return 0;
  using namespace trajectory::evolution::integration;
  std::string consumer;
  te_client client{std::getenv("TRAJECTORY_EVOLUTION_TRIAL_URL"),
      std::getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN"), 5000, 2097152, 0};
  auto invoke = [&](const char* operation, std::size_t index) {
    const auto body = Json{{"operation", operation}, {"request_id", consumer + "-" + std::to_string(index)},
        {"consumer", consumer}, {"index", index}};
    auto request = body;
    if(std::strcmp(operation, "evaluation.fail") == 0) request.fields().erase("index");
    char* bytes = nullptr; long status = 0; const char* error = nullptr;
    const int result = te_invoke(&client, request.dump_for_transport().c_str(), &bytes, &status, &error);
    std::unique_ptr<char, decltype(&te_free)> owned(bytes, &te_free);
    require(!result && bytes, "evaluation_entry_transport_failed");
    const auto response = Json::parse(bytes);
    require(response.flag("ok", false), "evaluation_entry_transport_failed");
    return response["result"];
  };
  try {
    require(receiver && client.url && client.token && te_initialize() == 0, "evaluation_entry_initialization_failed");
    unsigned char salt[16]; require(RAND_bytes(salt, sizeof(salt)) == 1, "random_unavailable");
    const char* hex = "0123456789abcdef";
    for(auto b : salt) { consumer += hex[b >> 4]; consumer += hex[b & 15]; }
    for(std::size_t index = 0;;) {
      const auto packet = invoke("evaluation.next", index);
      const auto state = packet.str("state");
      if(state == "done") return 1;
      if(state == "waiting") { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
      require(state == "input" && packet.num("index", SIZE_MAX) == index, "evaluation_entry_response_invalid");
      require(receiver(packet["input"].dump_for_transport().c_str(), context) == 0, "evaluation_entry_handler_failed");
      ++index;
    }
  } catch(...) {
    if(!consumer.empty()) try { invoke("evaluation.fail", 0); } catch(...) {}
    return -1;
  }
}
