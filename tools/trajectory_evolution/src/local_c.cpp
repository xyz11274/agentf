#include "trajectory_evolution/local_c.h"
#include "trajectory_evolution/local_runtime.h"
#include "integration_json.h"
#include "instrumentation.h"
#include "../clients/c/evolution.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <chrono>
#include <set>
#include <openssl/rand.h>
#include <openssl/sha.h>

using trajectory::evolution::LocalRuntime;
using namespace trajectory::evolution::integration;
struct te_local_handle {
  LocalRuntime runtime;
  std::size_t limit;
  te_local_handle(const char* path, const char* token, trajectory::evolution::LocalRuntimeOptions options)
      : runtime(path, token, options), limit(options.max_request_bytes) {}
};
namespace {
char* copy(const std::string& value) noexcept {
  auto* result = static_cast<char*>(std::malloc(value.size() + 1));
  if(result) std::memcpy(result, value.c_str(), value.size() + 1);
  return result;
}
}
extern "C" {
te_local_handle* te_local_open(const char* config, const char* token, const char* options_json, char** error) {
  if(error) *error = nullptr;
  try {
    require(config && token, "local_configuration_missing");
    const auto value = Json::parse(options_json && *options_json ? options_json : "{}");
    keys(value, {"max_request_bytes", "max_buffer_bytes", "max_pending_runs", "poll_interval_ms", "background", "contention_budget_us"});
    trajectory::evolution::LocalRuntimeOptions options;
    options.max_request_bytes = value.num("max_request_bytes", options.max_request_bytes);
    options.max_buffer_bytes = value.num("max_buffer_bytes", options.max_buffer_bytes);
    options.max_pending_runs = value.num("max_pending_runs", options.max_pending_runs);
    options.poll_interval_ms = value.num("poll_interval_ms", options.poll_interval_ms);
    options.background = value.flag("background", options.background);
    const auto contention = value.num("contention_budget_us", options.contention_budget_us);
    require(contention <= 10000, "local_limits_invalid"); options.contention_budget_us = static_cast<unsigned>(contention);
    return new te_local_handle(config, token, options);
  } catch(...) { if(error) *error = copy("local_initialization_failed"); return nullptr; }
}
char* te_local_invoke(te_local_handle* handle, const char* request, size_t size) {
  try {
    if(!handle || !request) return copy(R"({"ok":false,"error":"local_unavailable"})");
    if(size > handle->limit) {
      handle->runtime.invoke("");
      return copy(R"({"ok":true,"result":{"degraded":true,"reason":"local_request_too_large","capture_complete":false,"policy_applied":false}})");
    }
    return copy(handle->runtime.invoke(std::string(request, size)).body);
  } catch(...) { return copy(R"({"ok":false,"error":"local_unavailable"})"); }
}
char* te_local_metrics(te_local_handle* handle) {
  try { return copy(handle ? handle->runtime.metrics() : "{}"); } catch(...) { return nullptr; }
}
char* te_remote_point(const char* origin, const char* token, const char* request, size_t size, unsigned timeout_ms) {
  Json packet = Json::Object{};
  static std::timed_mutex gap_mutex;
  static std::set<std::string> gaps;
  static std::atomic<bool> overflow{false};
  std::string gap_key;
  auto observed = [&](const Json& result) {
    if(gap_key.empty()) return;
    std::unique_lock<std::timed_mutex> guard(gap_mutex, std::defer_lock);
    if(!guard.try_lock_for(std::chrono::microseconds(1000))) { overflow = true; return; }
    if(result.flag("degraded", false)) {
      if(gaps.size() < 4096) gaps.insert(gap_key); else overflow = true;
    } else if(result.flag("closed", false) || result.flag("run_closed", false)) gaps.erase(gap_key);
  };
  try {
    require(request && size <= 2097152, "request_too_large");
    packet = Json::parse(std::string(request, size)); packet["operation"] = "point";
    if(packet.has("ref")) {
      const auto identity = std::string(origin ? origin : "") + "\n" + (token ? token : "") + "\n" + point_run_key(packet);
      unsigned char digest[SHA256_DIGEST_LENGTH];
      require(SHA256(reinterpret_cast<const unsigned char*>(identity.data()), identity.size(), digest), "hash_failed");
      gap_key.assign(reinterpret_cast<const char*>(digest), sizeof(digest));
      std::unique_lock<std::timed_mutex> guard(gap_mutex, std::defer_lock);
      if(!guard.try_lock_for(std::chrono::microseconds(1000))) overflow = true;
      if(overflow || (guard.owns_lock() && gaps.count(gap_key))) packet["capture_gap"] = true;
    }
    if(!packet.has("request_id")) {
      unsigned char bytes[16]; require(RAND_bytes(bytes, sizeof(bytes)) == 1, "request_id_unavailable");
      std::string id; const char* hex = "0123456789abcdef";
      for(auto byte : bytes) { id += hex[byte >> 4]; id += hex[byte & 15]; } packet["request_id"] = id;
    }
    const auto budget = timeout_ms ? timeout_ms : 20;
    require(budget <= 5000, "point_timeout_invalid");
    static std::once_flag initialized;
    std::call_once(initialized, [] { require(te_initialize() == 0, "transport_initialization_failed"); });
    te_client client{origin, token, static_cast<long>(budget), 2097152, 0};
    char* response = nullptr; long status = 0; const char* error = nullptr;
    const auto result = te_invoke(&client, packet.dump().c_str(), &response, &status, &error);
    std::unique_ptr<char, decltype(&te_free)> owned(response, te_free);
    require(result == 0 && response, "observer_unavailable");
    const auto decoded = Json::parse(response);
    require(decoded.flag("ok", false) && decoded["result"].object(), "response_invalid");
    observed(decoded["result"]);
    return copy(decoded.dump_for_transport());
  } catch(...) {
    observed(Json{{"degraded", true}});
    try { return copy(Json{{"ok", true}, {"result", point_fallback(packet, "observer_unavailable")}}.dump_for_transport()); }
    catch(...) { return copy(R"({"ok":true,"result":{"degraded":true,"capture_complete":false,"policy_applied":false}})"); }
  }
}
char* te_local_manage(te_local_handle* handle, const char* request, size_t size) {
  try {
    if(!handle || !request) return copy(R"({"ok":false,"error":"local_unavailable"})");
    if(size > handle->limit) return copy(R"({"ok":false,"error":"local_request_too_large"})");
    return copy(handle->runtime.manage(std::string(request, size)).body);
  } catch(...) { return copy(R"({"ok":false,"error":"local_management_unavailable"})"); }
}
void te_local_drain(te_local_handle* handle) { try { if(handle) handle->runtime.drain(); } catch(...) {} }
void te_local_set_enabled(te_local_handle* handle, int enabled) { if(handle) handle->runtime.set_enabled(enabled != 0); }
void te_local_close(te_local_handle* handle) { delete handle; }
void te_local_free(char* value) { std::free(value); }
}
