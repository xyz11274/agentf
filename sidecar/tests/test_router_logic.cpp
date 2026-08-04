#include <gtest/gtest.h>
#include <json/json.h>
#include <fstream>
#include <sstream>
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
    EXPECT_TRUE(should_forward_to_callback(result.source));
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

// ═══════════════════════════════════════════════════════════
// SSEChunkProcessor — chunk 决策器单元测试
// ═══════════════════════════════════════════════════════════

static Json::Value make_llm_chunk(const std::string& content, const std::string& source = "coordinator") {
    Json::Value sse;
    sse["choices"][0]["delta"]["content"] = content;
    sse["_chunk_type"] = "llm_chunk";
    sse["_source"] = source;
    return sse;
}

static Json::Value make_tool_calls_delta() {
    Json::Value sse;
    sse["choices"][0]["delta"]["content"] = Json::nullValue;
    sse["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] = "run_skills";
    sse["_chunk_type"] = "llm_chunk";
    sse["_source"] = "coordinator";
    return sse;
}

static Json::Value make_coord_progress(const std::string& text, const std::string& skill) {
    Json::Value sse;
    sse["choices"][0]["message"]["content"] = text;
    sse["_chunk_type"] = "progress";
    sse["_source"] = "coordinator";
    sse["_skill"] = skill;
    return sse;
}

static Json::Value make_skill_progress(const std::string& text, const std::string& skill, const std::string& round) {
    Json::Value sse;
    sse["choices"][0]["message"]["content"] = text;
    sse["_chunk_type"] = "progress";
    sse["_source"] = "skill_executor";
    sse["_skill"] = skill;
    sse["_round"] = round;
    return sse;
}

static Json::Value make_final_block(const std::string& content) {
    Json::Value sse;
    sse["choices"][0]["message"]["content"] = content;
    sse["_final"] = true;
    return sse;
}

TEST(SSEChunkProcessor, LlmChunkText_ForwardedAsFlag0) {
    SSEChunkProcessor proc;
    auto results = proc.process(make_llm_chunk("你好"));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].flag, 0);
    EXPECT_EQ(results[0].text, "你好");
    EXPECT_EQ(results[0].stream_status, "Running");
    EXPECT_FALSE(results[0].is_final);
}

TEST(SSEChunkProcessor, ToolCallsDelta_Skipped) {
    SSEChunkProcessor proc;
    auto results = proc.process(make_tool_calls_delta());
    EXPECT_TRUE(results.empty());
}

TEST(SSEChunkProcessor, SkillExecutor_LlmChunk_ForwardedAsFlag1_NotAccumulated) {
    SSEChunkProcessor proc;
    auto results = proc.process(make_llm_chunk("skill text", "skill_executor"));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].flag, 1);
    EXPECT_EQ(results[0].text, "skill text");
    EXPECT_EQ(results[0].stream_status, "Running");
    EXPECT_FALSE(results[0].is_final);
    // LLM 增量不累积进最终回复文本(避免污染 TaskStatusReport)
    EXPECT_TRUE(proc.accumulated_content().empty());
}

TEST(SSEChunkProcessor, SkillExecutor_Reasoning_Filtered) {
    Json::Value sse;
    sse["choices"][0]["delta"]["reasoning_content"] = "skill reasoning";
    sse["choices"][0]["delta"]["content"] = "skill text";
    sse["_chunk_type"] = "llm_chunk";
    sse["_source"] = "skill_executor";
    SSEChunkProcessor proc;
    auto results = proc.process(sse);
    // 只转发 content 增量,reasoning 不外露
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].flag, 1);
    EXPECT_EQ(results[0].text, "skill text");
}

TEST(SSEChunkProcessor, SkillExecutor_Progress_ForwardedAsFlag1) {
    SSEChunkProcessor proc;
    auto results = proc.process(make_skill_progress("outfit-stylist 第 1 轮：调用 LLM…", "outfit-stylist", "1"));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].flag, 1);
    EXPECT_EQ(results[0].text, "outfit-stylist 第 1 轮：调用 LLM…");
    EXPECT_EQ(results[0].stream_status, "Running");
    EXPECT_FALSE(results[0].is_final);
}

TEST(SSEChunkProcessor, SkillExecutor_Progress_EmptyText_Skipped) {
    SSEChunkProcessor proc;
    auto results = proc.process(make_skill_progress("", "outfit-stylist", "1"));
    EXPECT_TRUE(results.empty());
}

TEST(SSEChunkProcessor, CoordinatorProgress_ThinkFlag1) {
    SSEChunkProcessor proc;
    auto results = proc.process(make_coord_progress("coordinator 决策中…", "coordinator"));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].flag, 1);
    EXPECT_EQ(results[0].text, "coordinator 决策中…");
}

TEST(SSEChunkProcessor, DaemonPreview_NoSkill_Skipped) {
    Json::Value sse;
    sse["choices"][0]["message"]["content"] = "preview text";
    sse["_chunk_type"] = "progress";
    SSEChunkProcessor proc;
    auto results = proc.process(sse);
    EXPECT_TRUE(results.empty());
}

TEST(SSEChunkProcessor, Final_SavesTextNotForwarded) {
    SSEChunkProcessor proc;
    auto results = proc.process(make_final_block("最终回复文本"));
    EXPECT_TRUE(results.empty());

    auto completed = proc.make_completed();
    EXPECT_EQ(completed.text, "最终回复文本");
    EXPECT_EQ(completed.stream_status, "Completed");
    EXPECT_TRUE(completed.is_final);
}

TEST(SSEChunkProcessor, ReasoningAndToken_BothForwarded) {
    Json::Value sse;
    sse["choices"][0]["delta"]["reasoning_content"] = "思考过程";
    sse["choices"][0]["delta"]["content"] = "回复文本";
    sse["_chunk_type"] = "llm_chunk";
    sse["_source"] = "coordinator";

    SSEChunkProcessor proc;
    auto results = proc.process(sse);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].flag, 1);
    EXPECT_EQ(results[0].text, "思考过程");
    EXPECT_EQ(results[1].flag, 0);
    EXPECT_EQ(results[1].text, "回复文本");
}

TEST(SSEChunkProcessor, AccumulatedContent_OnlyLlmChunk) {
    SSEChunkProcessor proc;
    proc.process(make_coord_progress("决策中", "coordinator"));
    proc.process(make_llm_chunk("Hello"));
    proc.process(make_llm_chunk(" World"));
    EXPECT_EQ(proc.accumulated_content(), "Hello World");
}

// ═══════════════════════════════════════════════════════════
// SSEChunkProcessor — 全量 dump 数据回归测试
// 数据源：logs/sse_dump 真实 outfit-assistant 请求
// ═══════════════════════════════════════════════════════════

TEST(SSEChunkProcessor, FullDumpReplay) {
    std::string fixture_path = TEST_FIXTURES_DIR;
    fixture_path += "/sse_dump_outfit.jsonl";

    std::ifstream file(fixture_path);
    ASSERT_TRUE(file.is_open()) << "Cannot open fixture: " << fixture_path;

    SSEChunkProcessor proc;
    int total_events = 0;
    int flag0_count = 0;
    int flag1_count = 0;
    int empty_count = 0;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        Json::Value sse_json;
        Json::CharReaderBuilder reader;
        std::istringstream iss(line);
        std::string errs;
        if (!Json::parseFromStream(reader, iss, &sse_json, &errs)) continue;
        if (sse_json.get("_meta", false).asBool()) continue;

        total_events++;
        auto results = proc.process(sse_json);
        if (results.empty()) {
            empty_count++;
        } else {
            for (const auto& cb : results) {
                if (cb.flag == 0) flag0_count++;
                else if (cb.flag == 1) flag1_count++;
            }
        }
    }

    auto completed = proc.make_completed();

    EXPECT_EQ(total_events, 1288);
    EXPECT_EQ(flag0_count, 390);
    // 744 条 skill_executor progress 现转发为 flag=1(思考过程), coordinator progress 5 条
    EXPECT_EQ(flag1_count, 749);
    EXPECT_EQ(empty_count, 149);
    EXPECT_EQ(completed.text.size(), 2141u);  // 1043 Unicode chars = 2141 UTF-8 bytes
    EXPECT_EQ(completed.stream_status, "Completed");
    EXPECT_TRUE(completed.is_final);
    EXPECT_FALSE(proc.accumulated_content().empty());
}

TEST(SSEChunkProcessor, ArtifactsField_UnderscorePrefixed) {
    Json::Value sse;
    sse["choices"][0]["message"]["content"] = "最终回复";
    sse["_final"] = true;
    Json::Value arts(Json::arrayValue);
    Json::Value img;
    img["type"] = "image";
    img["url"] = "/api/artifacts/test/img.png";
    img["name"] = "img.png";
    arts.append(img);
    sse["_artifacts"] = arts;

    SSEChunkProcessor proc;
    proc.process(sse);

    auto completed = proc.make_completed();
    EXPECT_FALSE(completed.user_output_object.empty());
    EXPECT_NE(completed.user_output_object.find("img.png"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════
// SSEChunkProcessor — 带 artifacts 产物的全量 dump 回归测试
// 数据源：真实 outfit-assistant 请求（含 2 张穿搭效果图）
// ═══════════════════════════════════════════════════════════

TEST(SSEChunkProcessor, FullDumpReplay_WithArtifacts) {
    std::string fixture_path = TEST_FIXTURES_DIR;
    fixture_path += "/sse_dump_outfit_with_artifacts.jsonl";

    std::ifstream file(fixture_path);
    ASSERT_TRUE(file.is_open()) << "Cannot open fixture: " << fixture_path;

    SSEChunkProcessor proc;
    int total_events = 0;
    int flag0_count = 0;
    int flag1_count = 0;
    int empty_count = 0;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        Json::Value sse_json;
        Json::CharReaderBuilder reader;
        std::istringstream iss(line);
        std::string errs;
        if (!Json::parseFromStream(reader, iss, &sse_json, &errs)) continue;
        if (sse_json.get("_meta", false).asBool()) continue;

        total_events++;
        auto results = proc.process(sse_json);
        if (results.empty()) {
            empty_count++;
        } else {
            for (const auto& cb : results) {
                if (cb.flag == 0) flag0_count++;
                else if (cb.flag == 1) flag1_count++;
            }
        }
    }

    auto completed = proc.make_completed();

    EXPECT_EQ(total_events, 1082);
    EXPECT_EQ(flag0_count, 428);
    // 518 条 skill_executor progress 现转发为 flag=1(思考过程), coordinator progress 3 条
    EXPECT_EQ(flag1_count, 521);
    EXPECT_EQ(empty_count, 133);
    EXPECT_EQ(completed.text.size(), 2534u);  // 1324 Unicode chars = 2534 UTF-8 bytes

    // 关键验证：_artifacts 字段被正确读取并放入 user_output_object
    EXPECT_FALSE(completed.user_output_object.empty());
    EXPECT_NE(completed.user_output_object.find("outfit-1.png"), std::string::npos);
    EXPECT_NE(completed.user_output_object.find("outfit-2.png"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════
// SSEChunkProcessor — bridge SSE 错误信封检测
// 对齐 chat_rest.py:1004-1021：bridge 在 daemon 通知中途失败/timeout 时，
// HTTP 200 + SSE 推 {"error":{"message":"...","code":-32000}} + [DONE]。
// sidecar 必须识别此 payload、设置 has_error()，调用方据此跳过 Completed、改发错误 TaskStatus。
// 否则错误信息被静默丢弃、上游收到虚假 Completed + 200/TaskCompleted（F1 数据安全）。
// ═══════════════════════════════════════════════════════════

TEST(SSEChunkProcessor, BridgeErrorPayload_SetsHasError) {
    // 模拟 bridge SSE 错误信封
    Json::Value err_payload;
    err_payload["error"]["message"] = "agent.execute timed out after 30s";
    err_payload["error"]["code"] = -32000;

    SSEChunkProcessor proc;
    auto results = proc.process(err_payload);

    EXPECT_TRUE(proc.has_error());
    EXPECT_EQ(proc.error_message(), "agent.execute timed out after 30s");
    // 错误 payload 不应产生任何回调指令（should_send 全 false）
    EXPECT_EQ(results.size(), 0u);
}

TEST(SSEChunkProcessor, NormalChunk_NoErrorSet) {
    // 正常 LLM chunk 不应触发 has_error
    Json::Value normal;
    normal["choices"][0]["delta"]["content"] = "你好";
    normal["_source"] = "coordinator";
    normal["_chunk_type"] = "llm_chunk";

    SSEChunkProcessor proc;
    proc.process(normal);

    EXPECT_FALSE(proc.has_error());
    EXPECT_EQ(proc.error_message(), "");
}

TEST(SSEChunkProcessor, ErrorPayload_SkipsSubsequentChunks) {
    // 错误 payload 后的后续 chunk 应被丢弃（已错则不再处理）
    Json::Value err_payload;
    err_payload["error"]["message"] = "daemon error: connection lost";
    Json::Value late_chunk;
    late_chunk["choices"][0]["delta"]["content"] = "迟到的 chunk";
    late_chunk["_source"] = "coordinator";
    late_chunk["_chunk_type"] = "llm_chunk";

    SSEChunkProcessor proc;
    proc.process(err_payload);
    EXPECT_TRUE(proc.has_error());

    auto results = proc.process(late_chunk);
    EXPECT_EQ(results.size(), 0u);  // 已错状态丢弃后续 chunk
    // accumulated_content 不应被迟到的 chunk 污染
    EXPECT_EQ(proc.accumulated_content(), "");
}

TEST(SSEChunkProcessor, ErrorPayload_EmptyMessage_NotTreatedAsError) {
    // error 字段存在但 message 为空 — 不应误判为错误（保守处理，避免空 message 触发 false positive）
    Json::Value malformed;
    malformed["error"]["message"] = "";
    malformed["error"]["code"] = -32000;

    SSEChunkProcessor proc;
    proc.process(malformed);

    EXPECT_FALSE(proc.has_error());
}
