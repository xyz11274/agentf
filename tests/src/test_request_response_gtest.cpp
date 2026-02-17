// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test tests for Request/Response functionality

#include <gtest/gtest.h>

#include "agentflow/common.h"
#include "agentflow/network/request.h"
#include "agentflow/network/response.h"

namespace agentflow {
namespace tests {

class RequestResponseTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// ============================================================================
// Request Creation Tests
// ============================================================================

TEST_F(RequestResponseTest, CreateRequestDefault) {
  network::Request request("execute_workflow");

  EXPECT_EQ(request.get_type(), "execute_workflow");
  EXPECT_FALSE(request.get_request_id().empty());
  EXPECT_TRUE(request.is_valid());
}

TEST_F(RequestResponseTest, CreateRequestWithType) {
  network::Request request("get_status");

  EXPECT_EQ(request.get_type(), "get_status");
  EXPECT_EQ(request.get_metadata().request_type, "get_status");
}

TEST_F(RequestResponseTest, CreateRequestWithPayload) {
  DataMap payload;
  payload["key1"] = static_cast<int64_t>(123);
  payload["key2"] = std::string("value");

  network::RequestMetadata metadata;
  network::Request         request("execute_workflow", payload, metadata);

  EXPECT_EQ(request.get_payload().size(), 2);
  EXPECT_EQ(std::get<int64_t>(request.get("key1")), 123);
  EXPECT_EQ(std::get<std::string>(request.get("key2")), "value");
}

TEST_F(RequestResponseTest, CreateRequestWithMetadata) {
  network::RequestMetadata metadata;
  metadata.source_id   = "source_001";
  metadata.target_id   = "target_001";
  metadata.workflow_id = "workflow_001";
  metadata.timeout_ms  = 5000;

  network::Request request("execute_workflow", DataMap(), metadata);

  EXPECT_EQ(request.get_metadata().source_id, "source_001");
  EXPECT_EQ(request.get_metadata().target_id, "target_001");
  EXPECT_EQ(request.get_metadata().workflow_id, "workflow_001");
  EXPECT_EQ(request.get_metadata().timeout_ms, 5000);
}

// ============================================================================
// RequestBuilder Tests
// ============================================================================

TEST_F(RequestResponseTest, RequestBuilderBasic) {
  network::RequestBuilder builder;

  auto request = builder.build();

  EXPECT_TRUE(request.is_valid());
  EXPECT_FALSE(request.get_request_id().empty());
}

TEST_F(RequestResponseTest, RequestBuilderChaining) {
  auto request = network::RequestBuilder()
                     .with_type("execute_workflow")
                     .with_data("input", std::string("test_value"))
                     .with_source("client_001")
                     .with_target("workflow_001")
                     .with_workflow("workflow_001")
                     .with_node("node_001")
                     .with_timeout(10000)
                     .with_header("Authorization", "Bearer token")
                     .build();

  EXPECT_EQ(request.get_type(), "execute_workflow");
  EXPECT_EQ(std::get<std::string>(request.get_payload()["input"]), "test_value");
  EXPECT_EQ(request.get_metadata().source_id, "client_001");
  EXPECT_EQ(request.get_metadata().target_id, "workflow_001");
  EXPECT_EQ(request.get_metadata().workflow_id, "workflow_001");
  EXPECT_EQ(request.get_metadata().node_id, "node_001");
  EXPECT_EQ(request.get_metadata().timeout_ms, 10000);
  EXPECT_EQ(request.get_header("Authorization"), "Bearer token");
  EXPECT_TRUE(request.is_valid());
}

TEST_F(RequestResponseTest, RequestBuilderWithMultipleHeaders) {
  auto request = network::RequestBuilder()
                     .with_type("execute_workflow")
                     .with_header("Content-Type", "application/json")
                     .with_header("Accept", "application/json")
                     .with_header("X-Custom-Header", "value")
                     .build();

  EXPECT_EQ(request.get_header("Content-Type"), "application/json");
  EXPECT_EQ(request.get_header("Accept"), "application/json");
  EXPECT_EQ(request.get_header("X-Custom-Header"), "value");
}

// ============================================================================
// Request Validation Tests
// ============================================================================

TEST_F(RequestResponseTest, ValidRequest) {
  network::Request request("test_type");
  request.set("key", std::string("value"));

  EXPECT_TRUE(request.is_valid());
}

TEST_F(RequestResponseTest, RequestHasData) {
  network::Request request("test_type");
  request.set("key1", static_cast<int64_t>(1));
  request.set("key2", std::string("value"));

  EXPECT_TRUE(request.has("key1"));
  EXPECT_TRUE(request.has("key2"));
  EXPECT_FALSE(request.has("nonexistent_key"));
}

TEST_F(RequestResponseTest, RequestGetData) {
  network::Request request("test_type");
  request.set("key", std::string("test_value"));

  auto value = request.get("key");
  EXPECT_EQ(std::get<std::string>(value), "test_value");
}

TEST_F(RequestResponseTest, RequestGetNonexistentData) {
  network::Request request("test_type");

  auto value = request.get("nonexistent_key");
  // Should return empty variant
  EXPECT_TRUE(std::holds_alternative<std::string>(value) || std::holds_alternative<int64_t>(value));
}

TEST_F(RequestResponseTest, RequestSetHeader) {
  network::Request request("test_type");

  request.set_header("Custom-Header", "custom_value");
  EXPECT_EQ(request.get_header("Custom-Header"), "custom_value");

  request.set_header("Another-Header", "another_value");
  EXPECT_EQ(request.get_header("Another-Header"), "another_value");
}

TEST_F(RequestResponseTest, RequestSetData) {
  network::Request request("test_type");

  request.set("key1", static_cast<int64_t>(1));
  request.set("key2", std::string("value"));

  EXPECT_EQ(std::get<int64_t>(request.get("key1")), 1);
  EXPECT_EQ(std::get<std::string>(request.get("key2")), "value");
}

// ============================================================================
// Request Serialization Tests (JSON)
// ============================================================================

TEST_F(RequestResponseTest, RequestToJson) {
  network::Request request("test_type");
  request.set("key", std::string("value"));

  auto json_str = request.to_json();

  EXPECT_FALSE(json_str.empty());
  EXPECT_TRUE(json_str.find("\"type\":\"test_type\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"key\":\"key\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"value\":\"value\"") != std::string::npos);
}

TEST_F(RequestResponseTest, RequestToJsonWithMetadata) {
  network::RequestBuilder builder;
  auto                    request = builder.with_type("execute_workflow")
                     .with_source("source_001")
                     .with_request_id("req_12345")
                     .build();

  auto json_str = request.to_json();

  EXPECT_TRUE(json_str.find("\"source_id\":\"source_001\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"request_id\":\"req_12345\"") != std::string::npos);
}

TEST_F(RequestResponseTest, RequestToJsonWithComplexData) {
  network::Request request("complex_type");

  DataMap payload;
  payload["string_val"] = std::string("hello");
  payload["int_val"]    = static_cast<int64_t>(42);
  payload["bool_val"]   = true;
  payload["double_val"] = 3.14;

  request.set("payload", payload);

  auto json_str = request.to_json();

  EXPECT_TRUE(json_str.find("\"payload\":{") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"string_val\":\"hello\"") != std::string::npos);
}

// ============================================================================
// Request ID Generation Tests
// ============================================================================

TEST_F(RequestResponseTest, GenerateRequestId) {
  auto id1 = network::Request::generate_request_id();
  auto id2 = network::Request::generate_request_id();

  EXPECT_FALSE(id1.empty());
  EXPECT_FALSE(id2.empty());
  EXPECT_NE(id1, id2);                 // IDs should be unique
  EXPECT_TRUE(id1.find("req_") == 0);  // IDs should have prefix
  EXPECT_TRUE(id2.find("req_") == 0);
}

// ============================================================================
// Response Creation Tests
// ============================================================================

TEST_F(RequestResponseTest, CreateResponseDefault) {
  network::Response response;

  EXPECT_EQ(response.get_status(), network::ResponseStatus::OK);
  EXPECT_EQ(response.get_status_message(), "OK");
  EXPECT_TRUE(response.is_success());
  EXPECT_FALSE(response.is_client_error());
  EXPECT_FALSE(response.is_server_error());
}

TEST_F(RequestResponseTest, CreateResponseOK) {
  auto response = network::Response::ok();

  EXPECT_EQ(response.get_status(), network::ResponseStatus::OK);
  EXPECT_EQ(response.get_status_message(), "OK");
  EXPECT_TRUE(response.is_success());
}

TEST_F(RequestResponseTest, CreateResponseCreated) {
  auto response = network::Response::created();

  EXPECT_EQ(response.get_status(), network::ResponseStatus::CREATED);
  EXPECT_EQ(response.get_status_message(), "Created");
  EXPECT_TRUE(response.is_success());
}

TEST_F(RequestResponseTest, CreateResponseBadRequest) {
  auto response = network::Response::bad_request("Invalid input");

  EXPECT_EQ(response.get_status(), network::ResponseStatus::BAD_REQUEST);
  EXPECT_EQ(response.get_status_message(), "Invalid input");
  EXPECT_TRUE(response.is_client_error());
  EXPECT_FALSE(response.is_success());
}

TEST_F(RequestResponseTest, CreateResponseNotFound) {
  auto response = network::Response::not_found("Resource not found");

  EXPECT_EQ(response.get_status(), network::ResponseStatus::NOT_FOUND);
  EXPECT_EQ(response.get_status_message(), "Resource not found");
  EXPECT_TRUE(response.is_client_error());
  EXPECT_FALSE(response.is_success());
}

TEST_F(RequestResponseTest, CreateResponseInternalError) {
  auto response = network::Response::internal_error("Internal error");

  EXPECT_EQ(response.get_status(), network::ResponseStatus::INTERNAL_ERROR);
  EXPECT_EQ(response.get_status_message(), "Internal error");
  EXPECT_TRUE(response.is_server_error());
  EXPECT_FALSE(response.is_success());
}

TEST_F(RequestResponseTest, CreateResponseServiceUnavailable) {
  auto response = network::Response::service_unavailable("Service unavailable");

  EXPECT_EQ(response.get_status(), network::ResponseStatus::SERVICE_UNAVAILABLE);
  EXPECT_EQ(response.get_status_message(), "Service unavailable");
  EXPECT_TRUE(response.is_server_error());
  EXPECT_FALSE(response.is_success());
}

// ============================================================================
// ResponseBuilder Tests
// ============================================================================

TEST_F(RequestResponseTest, ResponseBuilderBasic) {
  network::ResponseBuilder builder;

  auto response = builder.build();

  EXPECT_EQ(response.get_status(), network::ResponseStatus::OK);
  EXPECT_TRUE(response.is_success());
}

TEST_F(RequestResponseTest, ResponseBuilderChaining) {
  auto response = network::ResponseBuilder()
                      .with_status(network::ResponseStatus::OK)
                      .with_data("result", std::string("success"))
                      .with_request_id("req_123")
                      .with_status_message("Processed successfully")
                      .with_processing_time(42)
                      .with_header("X-Response-Id", "response_123")
                      .build();

  EXPECT_EQ(response.get_status(), network::ResponseStatus::OK);
  EXPECT_EQ(response.get_request_id(), "req_123");
  EXPECT_EQ(response.get_status_message(), "Processed successfully");
  EXPECT_EQ(std::get<std::string>(response.get_payload()["result"]), "success");
  EXPECT_EQ(response.get_metadata().processing_time_ms, 42);
  EXPECT_EQ(response.get_header("X-Response-Id"), "response_123");
  EXPECT_TRUE(response.is_success());
}

TEST_F(RequestResponseTest, ResponseBuilderWithMultipleHeaders) {
  auto response = network::ResponseBuilder()
                      .with_status(network::ResponseStatus::OK)
                      .with_header("Content-Type", "application/json")
                      .with_header("X-Custom-1", "value1")
                      .with_header("X-Custom-2", "value2")
                      .build();

  EXPECT_EQ(response.get_header("Content-Type"), "application/json");
  EXPECT_EQ(response.get_header("X-Custom-1"), "value1");
  EXPECT_EQ(response.get_header("X-Custom-2"), "value2");
}

TEST_F(ResponseResponseTest, ResponseBuilderWithError) {
  auto response = network::ResponseBuilder()
                      .with_status(network::ResponseStatus::BAD_REQUEST)
                      .with_status_message("Validation failed")
                      .with_error_code("VALIDATION_ERROR")
                      .with_error_detail("Field 'name' is required")
                      .build();

  EXPECT_EQ(response.get_status(), network::ResponseStatus::BAD_REQUEST);
  EXPECT_EQ(response.get_status_message(), "Validation failed");
  EXPECT_EQ(response.get_error_code(), "VALIDATION_ERROR");
  EXPECT_EQ(response.get_error_detail(), "Field 'name' is required");
  EXPECT_TRUE(response.is_client_error());
}

// ============================================================================
// Response Status Tests
// ============================================================================

TEST_F(RequestResponseTest, ResponseStatusCodes) {
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::OK), 200);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::CREATED), 201);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::ACCEPTED), 202);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::NO_CONTENT), 204);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::BAD_REQUEST), 400);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::UNAUTHORIZED), 401);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::FORBIDDEN), 403);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::NOT_FOUND), 404);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::CONFLICT), 409);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::UNPROCESSABLE_ENTITY), 422);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::INTERNAL_ERROR), 500);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::NOT_IMPLEMENTED), 501);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::BAD_GATEWAY), 502);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::SERVICE_UNAVAILABLE), 503);
  EXPECT_EQ(static_cast<int>(network::ResponseStatus::GATEWAY_TIMEOUT), 504);
}

TEST_F(RequestResponseTest, ResponseStatusClassification) {
  auto ok        = network::Response::ok();
  auto bad       = network::Response::bad_request();
  auto not_found = network::Response::not_found();
  auto internal  = network::Response::internal_error();

  // 2xx - Success
  EXPECT_TRUE(ok.is_success());
  EXPECT_TRUE(network::Response::created().is_success());

  // 4xx - Client errors
  EXPECT_TRUE(bad.is_client_error());
  EXPECT_TRUE(not_found.is_client_error());

  // 5xx - Server errors
  EXPECT_TRUE(internal.is_server_error());
}

TEST_F(RequestResponseTest, ResponseStatusRanges) {
  auto ok       = network::Response::ok();
  auto internal = network::Response::internal_error();

  int ok_code       = static_cast<int>(ok.get_status());
  int internal_code = static_cast<int>(internal.get_status());

  // Success codes are 2xx
  EXPECT_GE(ok_code, 200);
  EXPECT_LT(ok_code, 300);

  // Server errors are 5xx
  EXPECT_GE(internal_code, 500);
  EXPECT_LT(internal_code, 600);
}

// ============================================================================
// Response Payload Tests
// ============================================================================

TEST_F(RequestResponseTest, ResponseHasPayload) {
  network::Response response;
  response.set("key", std::string("value"));

  EXPECT_TRUE(response.has_payload());
  EXPECT_FALSE(response.get_payload().empty());
}

TEST_F(RequestResponseTest, ResponseNoPayload) {
  auto response = network::Response::ok();

  EXPECT_FALSE(response.has_payload());
  EXPECT_TRUE(response.get_payload().empty());
}

TEST_F(RequestResponseTest, ResponseGetPayloadData) {
  network::Response response;
  DataMap           payload;
  payload["result"] = std::string("success");
  payload["count"]  = static_cast<int64_t>(42);
  response.set("payload", payload);

  EXPECT_EQ(std::get<std::string>(response.get("result")), "success");
  EXPECT_EQ(std::get<int64_t>(response.get("count")), 42);
}

TEST_F(RequestResponseTest, ResponseGetNonexistentPayloadData) {
  network::Response response = network::Response::ok();

  auto value = response.get("nonexistent_key");
  // Should return empty variant
  EXPECT_TRUE(std::holds_alternative<std::string>(value) || std::holds_alternative<int64_t>(value));
}

TEST_F(RequestResponseTest, ResponseSetPayloadData) {
  network::Response response;
  response.set("key1", std::string("value1"));
  response.set("key2", static_cast<int64_t>(2));

  EXPECT_EQ(std::get<std::string>(response.get("key1")), "value1");
  EXPECT_EQ(std::get<int64_t>(response.get("key2")), 2);
}

TEST_F(RequestResponseTest, ResponseSetComplexPayload) {
  network::Response response;

  DataMap payload;
  payload["string_val"] = std::string("hello");
  payload["int_val"]    = static_cast<int64_t>(42);
  payload["bool_val"]   = true;
  payload["double_val"] = 3.14;

  response.set("payload", payload);

  EXPECT_EQ(std::get<std::string>(response.get("string_val")), "hello");
  EXPECT_EQ(std::get<int64_t>(response.get("int_val")), 42);
  EXPECT_EQ(std::get<bool>(response.get("bool_val")), true);
}

// ============================================================================
// Response Metadata Tests
// ============================================================================

TEST_F(RequestResponseTest, ResponseSetRequestId) {
  network::Response response;
  response.set_request_id("test_request_123");

  EXPECT_EQ(response.get_request_id(), "test_request_123");
}

TEST_F(RequestResponseTest, ResponseSetStatusMessage) {
  network::Response response;
  response.set_status_message("Custom status message");

  EXPECT_EQ(response.get_status_message(), "Custom status message");
}

TEST_F(RequestResponseTest, ResponseSetErrorCode) {
  network::Response response;
  response.set_error_code("ERR_001");

  EXPECT_EQ(response.get_error_code(), "ERR_001");
}

TEST_F(RequestResponseTest, ResponseSetErrorDetail) {
  network::Response response;
  response.set_error_detail("Detailed error description");

  EXPECT_EQ(response.get_error_detail(), "Detailed error description");
}

TEST_F(RequestResponseTest, ResponseSetHeader) {
  network::Response response;
  response.set_header("Response-Header", "response_value");

  EXPECT_EQ(response.get_header("Response-Header"), "response_value");
}

TEST_F(RequestResponseTest, ResponseSetProcessingTime) {
  network::Response response;
  response.set_processing_time(123);

  EXPECT_EQ(response.get_metadata().processing_time_ms, 123);
}

// ============================================================================
// Response Serialization Tests (JSON)
// ============================================================================

TEST_F(RequestResponseTest, ResponseToJson) {
  network::Response response;
  response.set("result", std::string("success"));

  auto json_str = response.to_json();

  EXPECT_FALSE(json_str.empty());
  EXPECT_TRUE(json_str.find("\"status\":200") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"result\":\"success\"") != std::string::npos);
}

TEST_F(RequestResponseTest, ResponseToJsonWithMetadata) {
  network::ResponseBuilder builder;
  auto                     response = builder.with_request_id("req_999")
                      .with_status_message("All good")
                      .with_error_code("OK")
                      .build();

  auto json_str = response.to_json();

  EXPECT_TRUE(json_str.find("\"request_id\":\"req_999\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"status_message\":\"All good\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"error_code\":\"OK\"") != std::string::npos);
}

TEST_F(RequestResponseTest, ResponseToJsonWithComplexData) {
  network::Response response;

  DataMap payload;
  payload["string_val"] = std::string("hello");
  payload["int_val"]    = static_cast<int64_t>(42);
  payload["bool_val"]   = true;
  payload["double_val"] = 3.14;

  response.set("payload", payload);

  auto json_str = response.to_json();

  EXPECT_TRUE(json_str.find("\"payload\":{") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"string_val\":\"hello\"") != std::string::npos);
}

TEST_F(RequestResponseTest, ResponseToJsonWithHeaders) {
  network::ResponseBuilder builder;
  auto                     response = builder.with_status(network::ResponseStatus::OK)
                      .with_header("X-Custom-Header", "value")
                      .build();

  auto json_str = response.to_json();

  EXPECT_TRUE(json_str.find("\"X-Custom-Header\":\"value\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"status\":200") != std::string::npos);
}

// ============================================================================
// Static Factory Method Tests
// ============================================================================

TEST_F(RequestResponseTest, ResponseOkStatic) {
  auto ok = network::Response::ok();

  EXPECT_EQ(ok.get_status(), network::ResponseStatus::OK);
  EXPECT_TRUE(ok.is_success());
}

TEST_F(RequestResponseTest, ResponseCreatedStatic) {
  auto created = network::Response::created();

  EXPECT_EQ(created.get_status(), network::ResponseStatus::CREATED);
  EXPECT_TRUE(created.is_success());
}

TEST_F(RequestResponseTest, ResponseBadRequestStatic) {
  auto bad = network::Response::bad_request();

  EXPECT_EQ(bad.get_status(), network::ResponseStatus::BAD_REQUEST);
  EXPECT_TRUE(bad.is_client_error());
}

TEST_F(RequestResponseTest, ResponseNotFoundStatic) {
  auto not_found = network::Response::not_found();

  EXPECT_EQ(not_found.get_status(), network::ResponseStatus::NOT_FOUND);
  EXPECT_TRUE(not_found.is_client_error());
}

TEST_F(RequestResponseTest, ResponseInternalErrorStatic) {
  auto internal = network::Response::internal_error();

  EXPECT_EQ(internal.get_status(), network::ResponseStatus::INTERNAL_ERROR);
  EXPECT_TRUE(internal.is_server_error());
}

TEST_F(RequestResponseTest, ResponseServiceUnavailableStatic) {
  auto unavailable = network::Response::service_unavailable();

  EXPECT_EQ(unavailable.get_status(), network::ResponseStatus::SERVICE_UNAVAILABLE);
  EXPECT_TRUE(unavailable.is_server_error());
}

// ============================================================================
// Builder Pattern Tests
// ============================================================================

TEST_F(RequestResponseTest, RequestBuilderShared) {
  auto shared = network::RequestBuilder().with_type("shared_test").build_shared();

  auto request = shared;
  EXPECT_TRUE(request.is_valid());
  EXPECT_EQ(request.get_type(), "shared_test");
}

TEST_F(RequestResponseTest, ResponseBuilderShared) {
  auto shared = network::ResponseBuilder().with_status(network::ResponseStatus::OK).build_shared();

  auto response = shared;
  EXPECT_TRUE(response.is_success());
  EXPECT_EQ(response.get_status(), network::ResponseStatus::OK);
}

// ============================================================================
// Request Type Constants Tests
// ============================================================================

TEST_F(RequestResponseTest, RequestTypeConstants) {
  EXPECT_STREQ(network::RequestTypes::EXECUTE_WORKFLOW, "execute_workflow");
  EXPECT_STREQ(network::RequestTypes::EXECUTE_NODE, "execute_node");
  EXPECT_STREQ(network::RequestTypes::GET_STATUS, "get_status");
  EXPECT_STREQ(network::RequestTypes::HEALTH_CHECK, "health_check");
  EXPECT_STREQ(network::RequestTypes::GET_CONFIG, "get_config");
  EXPECT_STREQ(network::RequestTypes::UPDATE_CONFIG, "update_config");
  EXPECT_STREQ(network::RequestTypes::STOP_WORKFLOW, "stop_workflow");
  EXPECT_STREQ(network::RequestTypes::STOP_NODE, "stop_node");
  EXPECT_STREQ(network::RequestTypes::GET_METRICS, "get_metrics");
  EXPECT_STREQ(network::RequestTypes::GET_LOGS, "get_logs");
}

}  // namespace tests
}  // namespace agentflow
