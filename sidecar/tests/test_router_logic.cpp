#include <gtest/gtest.h>
#include <json/json.h>
#include "router_logic.h"

using namespace sidecar;

// ═══════════════════════════════════════════════════════════
// EventType 校验 (B1)
// ═══════════════════════════════════════════════════════════

TEST(TestEventType, TC_SID_001_TaskAssignmentAccepted) {
    EXPECT_TRUE(is_supported_message_type("UserTaskManagement", "TaskAssignment"));
}

TEST(TestEventType, TC_SID_002_UserInputAccepted) {
    EXPECT_TRUE(is_supported_message_type("DataExchange", "UserInput"));
}

TEST(TestEventType, TC_SID_003_TaskStopRejected) {
    EXPECT_FALSE(is_supported_message_type("UserTaskManagement", "TaskStop"));
}

TEST(TestEventType, TC_SID_004_DataExchangeRejected) {
    EXPECT_FALSE(is_supported_message_type("TaskStatusManagement", "TaskStatusReport"));
    EXPECT_FALSE(is_supported_message_type("LifecycleManagement", "LoginOut"));
    EXPECT_FALSE(is_supported_message_type("", ""));
}

// ═══════════════════════════════════════════════════════════
// UserInputObject 解析 (B10) — stringified JSON
// ═══════════════════════════════════════════════════════════

TEST(TestInputFormat, TC_SID_020_StringifiedJsonParsed) {
    std::string obj_str = R"({"data":{"content":[{"type":"image","data":"base64_abc"}]}})";
    auto messages = build_openai_messages("hello", obj_str);

    auto content = messages[0]["content"];
    ASSERT_EQ(content.size(), 2);
    EXPECT_EQ(content[0]["type"].asString(), "text");
    EXPECT_EQ(content[0]["text"].asString(), "hello");
    EXPECT_EQ(content[1]["type"].asString(), "image_url");
    EXPECT_EQ(content[1]["image_url"]["url"].asString(), "base64_abc");
}

TEST(TestInputFormat, TC_SID_021_EmptyStringNoCrash) {
    auto messages = build_openai_messages("hello", "");

    auto content = messages[0]["content"];
    ASSERT_EQ(content.size(), 1);
    EXPECT_EQ(content[0]["type"].asString(), "text");
}

TEST(TestInputFormat, TC_SID_022_NonJsonStringNoCrash) {
    auto messages = build_openai_messages("hello", "not json at all");

    auto content = messages[0]["content"];
    ASSERT_EQ(content.size(), 1);
    EXPECT_EQ(content[0]["type"].asString(), "text");
}

TEST(TestInputFormat, TC_SID_007_MultimodalMerge) {
    std::string obj_str = R"({"data":{"content":[
        {"type":"image","data":"url1"},
        {"type":"image","data":"url2"}
    ]}})";
    auto messages = build_openai_messages("show me these", obj_str);

    auto content = messages[0]["content"];
    ASSERT_EQ(content.size(), 3);
    EXPECT_EQ(content[0]["type"].asString(), "text");
    EXPECT_EQ(content[1]["image_url"]["url"].asString(), "url1");
    EXPECT_EQ(content[2]["image_url"]["url"].asString(), "url2");
}

TEST(TestInputFormat, TC_SID_008_TextOnly) {
    auto messages = build_openai_messages("just text", "");

    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages[0]["role"].asString(), "user");
    auto content = messages[0]["content"];
    ASSERT_EQ(content.size(), 1);
    EXPECT_EQ(content[0]["type"].asString(), "text");
}

TEST(TestInputFormat, EmptyTextWithImage) {
    std::string obj_str = R"({"data":{"content":[{"type":"image","data":"only_image"}]}})";
    auto messages = build_openai_messages("", obj_str);

    auto content = messages[0]["content"];
    ASSERT_EQ(content.size(), 1);
    EXPECT_EQ(content[0]["type"].asString(), "image_url");
}

// ═══════════════════════════════════════════════════════════
// 回调 UserOutput 格式 (B11, B13)
// ═══════════════════════════════════════════════════════════

TEST(TestOutputFormat, TC_SID_023_HasUserOutputObject) {
    auto data = build_user_output_data("Running", 1, "hello");

    EXPECT_EQ(data["MessageType"].asString(), "UserOutput");
    EXPECT_TRUE(data.isMember("UserOutputObject"));
    EXPECT_EQ(data["UserOutputObject"].asString(), "");
}

TEST(TestOutputFormat, TC_SID_025_HasOptionalFields) {
    auto data = build_user_output_data("Completed", 0, "done");

    EXPECT_TRUE(data.isMember("UserManualInterventionFlag"));
    EXPECT_TRUE(data.isMember("ImplicitOutputFlag"));
    EXPECT_TRUE(data.isMember("IsVoiceBroadcast"));
    EXPECT_EQ(data["IsVoiceBroadcast"].asInt(), 1);
    EXPECT_FALSE(data["ImplicitOutputFlag"].asBool());
}

TEST(TestOutputFormat, TC_SID_024_TaskStatusCodeIsInt) {
    auto data = build_task_status_data(200, "TaskCompleted", "result text");

    EXPECT_EQ(data["MessageType"].asString(), "TaskStatusReport");
    EXPECT_EQ(data["TaskStatusCode"].asInt(), 200);
    EXPECT_EQ(data["TaskStatusReport"].asString(), "TaskCompleted");
    EXPECT_EQ(data["UserOutputText"].asString(), "result text");
}

TEST(TestOutputFormat, RunningStatusFields) {
    auto data = build_user_output_data("Running", 1, "partial");

    EXPECT_EQ(data["StreamStatus"].asString(), "Running");
    EXPECT_EQ(data["UserOutputFlag"].asInt(), 1);
    EXPECT_EQ(data["UserOutputText"].asString(), "partial");
}

TEST(TestOutputFormat, CompletedStatusFields) {
    auto data = build_user_output_data("Completed", 0, "");

    EXPECT_EQ(data["StreamStatus"].asString(), "Completed");
    EXPECT_EQ(data["UserOutputFlag"].asInt(), 0);
    EXPECT_EQ(data["UserOutputText"].asString(), "");
}

// ═══════════════════════════════════════════════════════════
// SSE 解析 (B6, B7)
// ═══════════════════════════════════════════════════════════

TEST(TestSSE, TC_SID_014_DoneTerminates) {
    auto result = parse_sse_line("data: [DONE]");

    EXPECT_TRUE(result.should_terminate);
    EXPECT_FALSE(result.should_process);
}

TEST(TestSSE, TC_SID_013_SkillExecutorFiltered) {
    std::string sse_line = R"(data: {"choices":[{"delta":{"content":"skip me"}}],"_source":"skill_executor"})";
    auto result = parse_sse_line(sse_line);

    ASSERT_TRUE(result.should_process);
    EXPECT_EQ(result.token, "skip me");
    EXPECT_EQ(result.source, "skill_executor");
    EXPECT_FALSE(should_forward_to_callback(result.source));
}

TEST(TestSSE, CoordinatorForwarded) {
    std::string sse_line = R"(data: {"choices":[{"delta":{"content":"hello"}}],"_source":"coordinator"})";
    auto result = parse_sse_line(sse_line);

    ASSERT_TRUE(result.should_process);
    EXPECT_EQ(result.token, "hello");
    EXPECT_EQ(result.source, "coordinator");
    EXPECT_TRUE(should_forward_to_callback(result.source));
}

TEST(TestSSE, EmptySourceForwarded) {
    std::string sse_line = R"(data: {"choices":[{"delta":{"content":"no source"}}]})";
    auto result = parse_sse_line(sse_line);

    ASSERT_TRUE(result.should_process);
    EXPECT_TRUE(result.source.empty());
    EXPECT_TRUE(should_forward_to_callback(result.source));
}

TEST(TestSSE, NonDataLineSkipped) {
    auto result = parse_sse_line("event: ping");
    EXPECT_FALSE(result.should_process);
    EXPECT_FALSE(result.should_terminate);
}

TEST(TestSSE, EmptyLineSkipped) {
    auto result = parse_sse_line("");
    EXPECT_FALSE(result.should_process);
}

TEST(TestSSE, InvalidJsonSkipped) {
    auto result = parse_sse_line("data: not json");
    EXPECT_FALSE(result.should_process);
}

TEST(TestSSE, MessageContentExtracted) {
    std::string sse_line = R"(data: {"choices":[{"message":{"content":"full response"}}]})";
    auto result = parse_sse_line(sse_line);

    ASSERT_TRUE(result.should_process);
    EXPECT_EQ(result.token, "full response");
}

// ═══════════════════════════════════════════════════════════
// Sender/Receiver 翻转 (B9)
// ═══════════════════════════════════════════════════════════

TEST(TestCallback, TC_SID_012_SenderReceiverSwap) {
    auto [new_sender, new_receiver] = swap_sender_receiver("APP", "SystemAgent");

    EXPECT_EQ(new_sender, "SystemAgent");
    EXPECT_EQ(new_receiver, "APP");
}

TEST(TestCallback, EmptyReceiverDefaultsSysAgent) {
    auto [new_sender, new_receiver] = swap_sender_receiver("APP", "");

    EXPECT_EQ(new_sender, "SysAgent");
    EXPECT_EQ(new_receiver, "APP");
}

TEST(TestCallback, EmptySenderDefaultsAPP) {
    auto [new_sender, new_receiver] = swap_sender_receiver("", "SystemAgent");

    EXPECT_EQ(new_sender, "SystemAgent");
    EXPECT_EQ(new_receiver, "APP");
}

// ═══════════════════════════════════════════════════════════
// 请求体解析 (TC-SID-005/006)
// ═══════════════════════════════════════════════════════════

TEST(TestParsing, TC_SID_005_EmptyBody) {
    auto result = parse_request_body("");
    EXPECT_FALSE(result.has_value());
}

TEST(TestParsing, TC_SID_006_InvalidJson) {
    EXPECT_FALSE(parse_request_body("not json").has_value());
    EXPECT_FALSE(parse_request_body("{broken").has_value());
    auto result = parse_request_body(R"({"key":"value"})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["key"].asString(), "value");
}

// ═══════════════════════════════════════════════════════════
// 健康检查响应 (TC-SID-015)
// ═══════════════════════════════════════════════════════════

TEST(TestHealth, TC_SID_015_Format) {
    auto body = build_health_response();
    EXPECT_EQ(body["status"].asString(), "healthy");
    EXPECT_EQ(body["coremind"].asBool(), true);
    EXPECT_EQ(body["sidecar"].asBool(), true);
}

// ═══════════════════════════════════════════════════════════
// 回调 Header 构建 + 透传字段 (TC-SID-011)
// ═══════════════════════════════════════════════════════════

TEST(TestCallback, TC_SID_011_PassthroughFields) {
    Json::Value req_header;
    req_header["AcmSessionId"] = "acm-123";
    req_header["DeviceId"] = "dev-456";
    req_header["SessionId"] = "sess-789";
    req_header["ExtraField"] = "should-not-passthrough";

    auto hdr = build_callback_header(
        "DataExchange", "task-1", "dlg-1", "user-1",
        "client-1", "APP", "SystemAgent", "seq-1", req_header
    );

    EXPECT_EQ(hdr["MessageType"].asString(), "DataExchange");
    EXPECT_EQ(hdr["TaskID"].asString(), "task-1");
    EXPECT_EQ(hdr["Sender"].asString(), "SystemAgent");
    EXPECT_EQ(hdr["Receiver"].asString(), "APP");
    EXPECT_EQ(hdr["AcmSessionId"].asString(), "acm-123");
    EXPECT_EQ(hdr["DeviceId"].asString(), "dev-456");
    EXPECT_EQ(hdr["SessionId"].asString(), "sess-789");
    EXPECT_FALSE(hdr.isMember("ExtraField"));
}

// ═══════════════════════════════════════════════════════════
// UserOutputObject 构建 (TC-SID-026)
// ═══════════════════════════════════════════════════════════

TEST(TestOutputFormat, TC_SID_026_ArtifactsToOutputObject) {
    Json::Value empty_artifacts(Json::arrayValue);
    EXPECT_EQ(build_user_output_object(empty_artifacts), "");
    EXPECT_EQ(build_user_output_object(Json::Value(Json::nullValue)), "");

    Json::Value artifacts(Json::arrayValue);
    Json::Value img;
    img["type"] = "image";
    img["url"] = "https://example.com/look.png";
    img["name"] = "look.png";
    img["mime_type"] = "image/png";
    artifacts.append(img);

    std::string result = build_user_output_object(artifacts);
    Json::Value parsed;
    Json::CharReaderBuilder reader;
    std::string err;
    std::istringstream ss(result);
    ASSERT_TRUE(Json::parseFromStream(reader, ss, &parsed, &err));
    EXPECT_EQ(parsed["code"].asInt(), 200101);
    EXPECT_EQ(parsed["data"]["content"][0]["type"].asString(), "image");
    EXPECT_EQ(parsed["data"]["content"][0]["url"].asString(), "https://example.com/look.png");
}
