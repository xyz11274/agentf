// Standalone TDD test for RapidJSON replacement of daemon RPC JSON parsing.
// Compile: g++ -std=c++17 -I ../../include -I ../../include/internal
//          -I ../../vendor/rapidjson/include -DAGENTFLOW_WITH_RAPIDJSON=1
//          test_rpc_json_parsing.cpp ../../vendor/... -o test_rpc_json_parsing
//
// This file re-implements the hand-written extract_json_string / extract_json_object
// functions from rpc_handler.cpp and rpc_server.cpp, but using RapidJSON.
// Test cases cover the exact edge cases that caused -32602 in production.

#include <cassert>
#include <cstdio>
#include <string>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

// ── Replacement implementations (RapidJSON-based) ─────────────────

static std::string rj_stringify(const rapidjson::Value& v) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  v.Accept(w);
  return buf.GetString();
}

// Like rpc_server.cpp's extract_json_string: handles quoted and unquoted values.
std::string extract_json_string(const std::string& json, const std::string& key) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember(key.c_str())) return "";
  const auto& v = d[key.c_str()];
  if (v.IsString()) return v.GetString();
  return rj_stringify(v);
}

// Like rpc_handler.cpp's extract_json_object_local: extracts nested JSON object.
std::string extract_json_object(const std::string& json, const std::string& key) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember(key.c_str()) || !d[key.c_str()].IsObject()) return "";
  return rj_stringify(d[key.c_str()]);
}

// Like rpc_server.cpp's extract_json_method.
std::string extract_json_method(const std::string& json) {
  return extract_json_string(json, "method");
}

// Like rpc_server.cpp's extract_json_id.
int64_t extract_json_id(const std::string& json) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember("id")) return -1;
  const auto& v = d["id"];
  if (v.IsInt64())  return v.GetInt64();
  if (v.IsInt())    return v.GetInt();
  if (v.IsUint64()) return static_cast<int64_t>(v.GetUint64());
  return -1;
}

// ── Test helpers ────────────────────────────────────────────────

int tests = 0, passed = 0;
#define TEST(name, expr) do { \
  ++tests; \
  bool ok = (expr); \
  printf("  %s %s\n", ok ? "✅" : "❌", name); \
  if (ok) ++passed; else printf("    FAIL: %s\n    file=%s line=%d\n", #expr, __FILE__, __LINE__); \
} while(0)

// ── Tests ───────────────────────────────────────────────────────

void test_extract_json_string_basic() {
  // Simple string value (quoted)
  std::string r1 = extract_json_string(R"({"method":"put","id":1})", "method");
  TEST("extract basic string", r1 == "put");

  // Number value (unquoted)
  std::string r2 = extract_json_string(R"({"id":42})", "id");
  TEST("extract unquoted number as string", r2 == "42");

  // Key not found
  std::string r3 = extract_json_string(R"({"a":1})", "nonexistent");
  TEST("extract missing key returns empty", r3.empty());

  // Empty JSON
  std::string r4 = extract_json_string("{}", "key");
  TEST("extract from empty object", r4.empty());

  // Invalid JSON
  std::string r5 = extract_json_string("{invalid", "key");
  TEST("extract from invalid JSON", r5.empty());
}

void test_extract_json_string_nested_key_collision() {
  // The critical bug: value contains a "key" field that shouldn't confuse extraction.
  // Top-level "key" = "traces/abc/meta", nested "key" = "some_value".
  std::string json = R"({"session_id":"0","namespace":"llm","key":"traces/abc/meta","value":{"model":"test","key":"nested_value"}})";
  std::string r = extract_json_string(json, "key");
  TEST("top-level key before nested key", r == "traces/abc/meta");

  // session_id collision: nested session_id inside value
  std::string json2 = R"({"session_id":"0","namespace":"llm","key":"traces/test/meta","value":{"model":"x","session_id":"0","node_id":"executor"}})";
  std::string r2 = extract_json_string(json2, "session_id");
  TEST("top-level session_id before nested session_id", r2 == "0");

  // namespace collision: nested namespace inside value
  std::string json3 = R"({"session_id":"0","namespace":"llm","key":"traces/test/meta","value":{"namespace":"nested_ns"}})";
  std::string r3 = extract_json_string(json3, "namespace");
  TEST("top-level namespace before nested", r3 == "llm");
}

void test_extract_json_string_with_special_chars() {
  // Chinese characters
  std::string r1 = extract_json_string(R"({"msg":"中文测试"})", "msg");
  TEST("Chinese characters", r1 == "中文测试");

  // String with escaped quotes
  std::string r2 = extract_json_string(R"({"msg":"say \"hello\""})", "msg");
  TEST("escaped quotes in value", r2 == "say \"hello\"");

  // Empty string value
  std::string r3 = extract_json_string(R"({"key":""})", "key");
  TEST("empty string value", r3 == "");

  // String with only whitespace
  std::string r4 = extract_json_string(R"({"key":"   "})", "key");
  TEST("whitespace-only value", r4 == "   ");

  // Very long string
  std::string long_val(10000, 'x');
  std::string json = "{\"key\":\"" + long_val + "\"}";
  std::string r5 = extract_json_string(json, "key");
  TEST("10k char string", r5 == long_val);
}

void test_extract_json_object() {
  // Basic object extraction
  std::string r1 = extract_json_object(R"({"params":{"session_id":"0","namespace":"llm"}})", "params");
  TEST("extract params object", !r1.empty() && r1.find("session_id") != std::string::npos);

  // Key not found
  std::string r2 = extract_json_object(R"({"a":1})", "params");
  TEST("missing object returns empty", r2.empty());

  // Value is not an object (it's a string)
  std::string r3 = extract_json_object(R"({"params":"not_an_object"})", "params");
  TEST("non-object value returns empty", r3.empty());

  // Nested object inside value
  std::string json = R"({"jsonrpc":"2.0","method":"put","params":{"session_id":"0","namespace":"llm","key":"traces/x/meta","value":{"model":"test","session_id":"0"}},"id":1})";
  std::string r4 = extract_json_object(json, "params");
  TEST("params with nested session_id inside value", 
       r4.find("\"session_id\":\"0\"") != std::string::npos &&
       r4.find("\"namespace\":\"llm\"") != std::string::npos &&
       r4.find("\"key\":\"traces") != std::string::npos);
}

void test_extract_json_method_and_id() {
  // Standard method/id
  std::string m1 = extract_json_method(R"({"jsonrpc":"2.0","method":"databus.get","params":{},"id":1})");
  TEST("method extraction", m1 == "databus.get");

  int64_t id1 = extract_json_id(R"({"jsonrpc":"2.0","method":"x","id":42})");
  TEST("id extraction", id1 == 42);

  // Missing method
  std::string m2 = extract_json_method(R"({"id":1})");
  TEST("missing method returns empty", m2.empty());

  // Missing id
  int64_t id2 = extract_json_id(R"({"method":"x"})");
  TEST("missing id returns -1", id2 == -1);

  // String id
  int64_t id3 = extract_json_id(R"({"id":"abc"})");
  TEST("string id returns -1", id3 == -1);

  // Large id
  int64_t id4 = extract_json_id(R"({"id":999999999999})");
  TEST("large id", id4 == 999999999999);
}

void test_full_json_rpc_roundtrip() {
  // Simulate a typical databus.get request from the bridge
  std::string req = R"({"jsonrpc":"2.0","method":"databus.get","params":{"agent_id":"outfit-assistant","session_id":"sess_abc","key":"llm/traces/index"},"id":1})";
  
  std::string method = extract_json_method(req);
  TEST("databus.get method", method == "databus.get");
  
  int64_t id = extract_json_id(req);
  TEST("databus.get id=1", id == 1);
  
  std::string params = extract_json_object(req, "params");
  TEST("databus.get params extracted", !params.empty());
  TEST("params has session_id", params.find("sess_abc") != std::string::npos);
  TEST("params has agent_id", params.find("outfit-assistant") != std::string::npos);
}

void test_bridge_trace_query() {
  // Simulate the exact flow: bridge sends databus.list_keys
  std::string req = R"({"jsonrpc":"2.0","method":"databus.list_keys","params":{"agent_id":"outfit-assistant","user_id":"web-user","client_id":"client_xxx","prefix":"llm/traces/"},"id":2})";
  
  std::string method = extract_json_method(req);
  TEST("list_keys method", method == "databus.list_keys");
  
  std::string params = extract_json_object(req, "params");
  TEST("list_keys params", !params.empty());
  TEST("prefix in params", params.find("llm/traces/") != std::string::npos);
}

// ── Main ─────────────────────────────────────────────────────────

int main() {
  printf("=== Daemon RPC JSON Parsing Tests (RapidJSON) ===\n\n");

  test_extract_json_string_basic();
  test_extract_json_string_nested_key_collision();
  test_extract_json_string_with_special_chars();
  test_extract_json_object();
  test_extract_json_method_and_id();
  test_full_json_rpc_roundtrip();
  test_bridge_trace_query();

  printf("\n=== Results: %d/%d passed ===\n", passed, tests);
  return passed == tests ? 0 : 1;
}
