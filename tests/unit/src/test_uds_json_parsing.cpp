// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

// Unit test for UDS server JSON parsing with nested JSON arrays/objects
// Tests the bug where extract_json_string truncates values at the first comma

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <thread>

#include "runtime/databus/databus.h"
#include "runtime/databus/uds_server.h"

using namespace agentflow;

class UDSJsonParsingTest : public ::testing::Test {
protected:
  runtime::MMDataBus databus_{1024 * 1024};
  std::unique_ptr<runtime::UDSServer> server_;
  std::string socket_path_;
  int client_fd_ = -1;

  void SetUp() override {
    socket_path_ = "/tmp/test_uds_json_" + std::to_string(getpid()) + "_" +
                   std::to_string(std::rand()) + ".sock";
    server_ = std::make_unique<runtime::UDSServer>(&databus_, socket_path_);
    ASSERT_TRUE(server_->start()) << "Failed to start UDS server";

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void TearDown() override {
    if (client_fd_ >= 0) {
      close(client_fd_);
    }
    server_.reset();
    unlink(socket_path_.c_str());
  }

  // Connect to UDS server
  bool connect_to_server() {
    client_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd_ < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    return connect(client_fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0;
  }

  // Send JSON-RPC request and get response
  std::string send_request(const std::string& request) {
    std::string msg = request + "\n";
    if (send(client_fd_, msg.c_str(), msg.size(), 0) < 0) {
      return "";
    }

    char buf[4096];
    ssize_t n = recv(client_fd_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return "";
    buf[n] = '\0';
    return std::string(buf);
  }
};

// Test that extract_json_string correctly handles JSON array values
// This tests the bug where nested JSON arrays were truncated at the first comma
TEST_F(UDSJsonParsingTest, PutNestedJsonArrayViaSocket) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // This is what a Python client would send - a JSON-RPC put with nested JSON array
  std::string request = R"({"jsonrpc":"2.0","method":"put","params":{"session_id":"test-session","namespace":"sys","key":"next_skills","value":[{"exec_skill_key":"aggregate_search","name":"search"},{"exec_skill_key":"surround_search","name":"surround-search"}]},"id":1})";

  std::string response = send_request(request);
  ASSERT_FALSE(response.empty()) << "Empty response";
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put failed: " << response;

  // Now get the value back and verify it wasn't truncated
  std::string get_request = R"({"jsonrpc":"2.0","method":"get","params":{"session_id":"test-session","namespace":"sys","key":"next_skills"},"id":2})";
  std::string get_response = send_request(get_request);

  // The value should contain the complete JSON array
  EXPECT_NE(get_response.find("aggregate_search"), std::string::npos)
      << "Missing aggregate_search in: " << get_response;
  EXPECT_NE(get_response.find("surround_search"), std::string::npos)
      << "Missing surround_search in: " << get_response;
  EXPECT_NE(get_response.find("\"search\""), std::string::npos)
      << "Missing 'search' in: " << get_response;
  EXPECT_NE(get_response.find("\"surround-search\""), std::string::npos)
      << "Missing 'surround-search' in: " << get_response;
}

// Test the specific bug case: JSON array with comma inside objects
TEST_F(UDSJsonParsingTest, JsonArrayWithCommaNotTruncated) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // The bug: extract_json_string stops at first comma inside nested JSON
  // Input: {"value":[{"key":"v1","name":"n1"}]}
  // Bug returns: [{"key":"v1"  (truncated at comma)
  // Expected: [{"key":"v1","name":"n1"}]

  std::string request = R"({"jsonrpc":"2.0","method":"put","params":{"session_id":"test","namespace":"sys","key":"test_key","value":[{"key":"v1","name":"n1"}]},"id":1})";

  std::string response = send_request(request);
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put failed: " << response;

  // Get and verify
  std::string get_request = R"({"jsonrpc":"2.0","method":"get","params":{"session_id":"test","namespace":"sys","key":"test_key"},"id":2})";
  std::string get_response = send_request(get_request);

  // Should contain both "key" and "name" fields
  EXPECT_NE(get_response.find("\"key\":\"v1\""), std::string::npos)
      << "Missing key field in: " << get_response;
  EXPECT_NE(get_response.find("\"name\":\"n1\""), std::string::npos)
      << "Missing name field (truncated at comma?): " << get_response;
}

// Test with the exact JSON that caused the bug report
TEST_F(UDSJsonParsingTest, ExactBugCaseFromLogs) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // From the bug report logs:
  // [LAUNCH_SKILL] Skill template not found: {"exec_skill_key":"aggregate_search"
  // [LAUNCH_SKILL] Skill template not found: "name":"search"}
  // This shows the JSON was split at the comma inside the object

  std::string request = R"({"jsonrpc":"2.0","method":"put","params":{"session_id":"test","namespace":"sys","key":"next_skills","value":[{"exec_skill_key":"aggregate_search","name":"search"},{"exec_skill_key":"surround_search","name":"surround-search"},{"exec_skill_key":"weather_query","name":"weather-query"}]},"id":1})";

  std::string response = send_request(request);
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put failed: " << response;

  // Get and verify all skills are present
  std::string get_request = R"({"jsonrpc":"2.0","method":"get","params":{"session_id":"test","namespace":"sys","key":"next_skills"},"id":2})";
  std::string get_response = send_request(get_request);

  // Each skill name should be present
  EXPECT_NE(get_response.find("\"search\""), std::string::npos)
      << "Missing 'search' skill in: " << get_response;
  EXPECT_NE(get_response.find("\"surround-search\""), std::string::npos)
      << "Missing 'surround-search' skill in: " << get_response;
  EXPECT_NE(get_response.find("\"weather-query\""), std::string::npos)
      << "Missing 'weather-query' skill in: " << get_response;

  // Each exec_skill_key should be present
  EXPECT_NE(get_response.find("aggregate_search"), std::string::npos)
      << "Missing 'aggregate_search' in: " << get_response;
  EXPECT_NE(get_response.find("surround_search"), std::string::npos)
      << "Missing 'surround_search' in: " << get_response;
  EXPECT_NE(get_response.find("weather_query"), std::string::npos)
      << "Missing 'weather_query' in: " << get_response;
}

// Test deeply nested JSON
TEST_F(UDSJsonParsingTest, DeeplyNestedJson) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // Test with more complex nested structure
  std::string request = R"({"jsonrpc":"2.0","method":"put","params":{"session_id":"test","namespace":"sys","key":"nested","value":{"level1":{"level2":{"data":"test","items":[1,2,3]}}}},"id":1})";

  std::string response = send_request(request);
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put failed: " << response;

  // Get and verify
  std::string get_request = R"({"jsonrpc":"2.0","method":"get","params":{"session_id":"test","namespace":"sys","key":"nested"},"id":2})";
  std::string get_response = send_request(get_request);

  // Should contain nested content
  EXPECT_NE(get_response.find("level1"), std::string::npos) << "Missing level1 in: " << get_response;
  EXPECT_NE(get_response.find("level2"), std::string::npos) << "Missing level2 in: " << get_response;
  EXPECT_NE(get_response.find("\"data\":\"test\""), std::string::npos)
      << "Missing data field in: " << get_response;
  EXPECT_NE(get_response.find("[1,2,3]"), std::string::npos)
      << "Missing items array in: " << get_response;
}

// Test single JSON object (not array) with comma inside
TEST_F(UDSJsonParsingTest, SingleJsonObjectWithComma) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // Test with a single JSON object value (not wrapped in array)
  // This is the exact case the user asked about:
  // {"exec_skill_key":"aggregate_search", "name":"search"}
  std::string request = R"({"jsonrpc":"2.0","method":"put","params":{"session_id":"test","namespace":"sys","key":"single_object","value":{"exec_skill_key":"aggregate_search","name":"search"}},"id":1})";

  std::string response = send_request(request);
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put failed: " << response;

  // Get and verify
  std::string get_request = R"({"jsonrpc":"2.0","method":"get","params":{"session_id":"test","namespace":"sys","key":"single_object"},"id":2})";
  std::string get_response = send_request(get_request);

  // Both fields should be present (not truncated at comma)
  EXPECT_NE(get_response.find("aggregate_search"), std::string::npos)
      << "Missing exec_skill_key in: " << get_response;
  EXPECT_NE(get_response.find("\"name\":\"search\""), std::string::npos)
      << "Missing name field (truncated at comma?): " << get_response;
}

// ── Regression tests for -32602 "missing session_id, namespace, or key" ──
// These test the bug where extract_json_string fails to find top-level keys
// when the nested `value` object also contains fields with the same name.
// All requests use single-line JSON to match what json.dumps() produces.

// Test: value dict contains "session_id" (like _task_context does in meta)
TEST_F(UDSJsonParsingTest, PutValueContainsSessionIdKey) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // Simulates meta dict that includes session_id via _task_context():
  //   meta = {"model": "...", ...}
  //   meta.update(_task_context())  → adds "session_id":"0"
  // This creates a nested "session_id" inside the value object that could
  // confuse extract_json_string("params", "session_id").
  std::string request = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/test123/meta\","
      + "\"value\":{\"model\":\"MiniMax-M3\",\"latency_ms\":1234,\"session_id\":\"0\",\"node_id\":\"skill_executor\"}"
      + "},\"id\":1}";

  std::string response = send_request(request);
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put failed (session_id in value confused extraction): " << response;
}

// Test: value dict contains "key" field (from LLM response or request)
TEST_F(UDSJsonParsingTest, PutValueContainsKeyField) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // Simulates a value that happens to contain a "key" field
  std::string request = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/test456/request\","
      + "\"value\":{\"model\":\"test\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"key\":\"some_value\"}"
      + "},\"id\":1}";

  std::string response = send_request(request);
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put failed (key in value confused extraction): " << response;
}

// Test: the exact _TraceRecorder put sequence (request + meta + response)
TEST_F(UDSJsonParsingTest, TraceRecorderPutSequence) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // Step 1: put request body (messages list with tools) – %zu stops at first comma.
  // Build programmatically to avoid newlines in R"()".
  std::string req1 = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/seq001/request\","
      + "\"value\":{\"model\":\"MiniMax-M3\",\"messages\":["
      + "{\"role\":\"system\",\"content\":\"you are a helper\"},"
      + "{\"role\":\"user\",\"content\":\"hi\"}"
      + "],\"max_completion_tokens\":2000,"
      + "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"run_skills\",\"description\":\"run\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}],"
      + "\"tool_choice\":\"auto\"}"
      + "},\"id\":1}";
  std::string r1 = send_request(req1);
  EXPECT_NE(r1.find("\"success\":true"), std::string::npos)
      << "Step1 put request failed: " << r1;

  // Step 2: put response (LLM API response with choices/usage)
  std::string req2 = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/seq001/response\","
      + "\"value\":{\"id\":\"chatcmpl-x\",\"object\":\"chat.completion\",\"created\":12345,"
      + "\"model\":\"MiniMax-M3\","
      + "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"hello\"},\"finish_reason\":\"stop\"}],"
      + "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}"
      + "},\"id\":2}";
  std::string r2 = send_request(req2);
  EXPECT_NE(r2.find("\"success\":true"), std::string::npos)
      << "Step2 put response failed: " << r2;

  // Step 3: put meta (with task_context fields including session_id)
  std::string req3 = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/seq001/meta\","
      + "\"value\":{"
      + "\"model\":\"MiniMax-M3\",\"latency_ms\":1234,\"timestamp\":\"2026-01-01T00:00:00Z\","
      + "\"request_summary\":\"system:..., user:...\",\"response_summary\":\"finish=stop\","
      + "\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15,"
      + "\"finish_reason\":\"stop\",\"msg_count\":2,\"tools_enabled\":true,\"retries_used\":0,"
      + "\"dialogue_id\":\"test-dlg\",\"session_id\":\"0\",\"node_id\":\"skill_executor\","
      + "\"task_id\":\"\",\"skill_name\":\"\",\"completed_skill\":\"\",\"task_type\":\"skill\""
      + "}"
      + "},\"id\":3}";
  std::string r3 = send_request(req3);
  EXPECT_NE(r3.find("\"success\":true"), std::string::npos)
      << "Step3 put meta failed: " << r3;

  // Step 4: get the meta back to verify it's stored correctly
  std::string req4 = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"get\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/seq001/meta\""
      + "},\"id\":4}";
  std::string r4 = send_request(req4);
  EXPECT_NE(r4.find("\"exists\":true"), std::string::npos)
      << "Step4 get meta failed (key not found): " << r4;
  EXPECT_NE(r4.find("\"node_id\":\"skill_executor\""), std::string::npos)
      << "Step4 get meta missing expected content: " << r4;
}

// Test: put with empty string values (edge case)
TEST_F(UDSJsonParsingTest, PutWithEmptyStringValues) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  std::string request = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/empty/meta\","
      + "\"value\":{"
      + "\"model\":\"\",\"latency_ms\":0,\"timestamp\":\"\","
      + "\"request_summary\":\"\",\"response_summary\":\"\","
      + "\"finish_reason\":\"\",\"session_id\":\"\",\"node_id\":\"\",\"task_type\":\"\""
      + "}"
      + "},\"id\":1}";

  std::string response = send_request(request);
  // Should succeed because session_id is "0", not empty
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put with empty string values failed: " << response;
}

// Test: large messages list (stress the parser)
TEST_F(UDSJsonParsingTest, PutWithLargeMessagesList) {
  ASSERT_TRUE(connect_to_server()) << "Failed to connect to UDS server";

  // Build a large messages list programmatically (single-line compact JSON)
  std::string value = "{\"model\":\"MiniMax-M3\",\"messages\":[";
  for (int i = 0; i < 20; i++) {
    if (i > 0) value += ",";
    value += "{\"role\":\"user\",\"content\":\"This is message " + std::to_string(i) + " with some Chinese text 中文测试 that exercises the parser\"}";
  }
  value += "],\"max_completion_tokens\":2000}";

  std::string request = std::string(
      "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":{")
      + "\"session_id\":\"0\",\"namespace\":\"llm\",\"key\":\"traces/large/request\","
      + "\"value\":" + value
      + "},\"id\":1}";

  std::string response = send_request(request);
  EXPECT_NE(response.find("\"success\":true"), std::string::npos)
      << "Put with large messages failed: " << response;
}