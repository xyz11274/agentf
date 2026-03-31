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