// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Tests for OpenAI-compatible SSE chunk parsing.
//
// Validates the parsing logic used by IMInferServiceClient::ParseOpenAIChunk()
// across multiple SSE formats: OpenAI standard streaming (delta),
// FusionMind wrapped (result.choices), non-streaming (message), tool_calls, usage, errors.
//
// These tests exercise the same parsing algorithm on the same input/output
// boundaries, without depending on SysAgent's HttpClient infrastructure.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

namespace agentflow {
namespace runtime {
namespace tests {

// ============================================================
// Parsed result (mirrors OpenAIChunkResult in IMInferServiceClient.h)
// ============================================================
struct ParseResult {
    std::string content;
    std::string finish_reason;
    std::string tool_calls_json;
    int prompt_tokens{0};
    int completion_tokens{0};
    int total_tokens{0};
    bool is_error{false};
    std::string error_message;
    bool is_done{false};
};

// ============================================================
// Parsing helper — pure function, same algorithm as ParseOpenAIChunk()
// ============================================================

constexpr char SSE_DATA_PREFIX[] = "data:";
constexpr char SSE_DONE_FLAG[] = "[DONE]";

static bool ParseOpenAIChunk(const std::string& sse_line, ParseResult& result) {
    // Skip "data: " prefix
    std::string_view view(sse_line);
    auto prefix_pos = view.find(SSE_DATA_PREFIX);
    if (prefix_pos != std::string::npos) {
        view.remove_prefix(prefix_pos + std::strlen(SSE_DATA_PREFIX));
    }
    // Trim leading whitespace
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t' ||
                             view.front() == '\r' || view.front() == '\n')) {
        view.remove_prefix(1);
    }
    if (view.empty()) return false;

    // Check [DONE]
    if (view == SSE_DONE_FLAG) {
        result.is_done = true;
        return true;
    }

    // Parse JSON
    rapidjson::Document doc;
    doc.Parse(view.data(), view.size());
    if (doc.HasParseError() || !doc.IsObject()) return false;

    // Error field
    if (doc.HasMember("error")) {
        result.is_error = true;
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        doc["error"].Accept(w);
        result.error_message = buf.GetString();
        return true;
    }

    // Locate choices[]
    const rapidjson::Value* choices = nullptr;
    if (doc.HasMember("choices") && doc["choices"].IsArray()) {
        choices = &doc["choices"];
    } else if (doc.HasMember("result") && doc["result"].IsObject()) {
        const auto& r = doc["result"];
        if (r.HasMember("choices") && r["choices"].IsArray()) {
            choices = &r["choices"];
        }
    }
    if (!choices || choices->Empty()) return true;  // empty choices, not an error

    const auto& choice0 = (*choices)[0];
    if (!choice0.IsObject()) return true;

    // finish_reason
    if (choice0.HasMember("finish_reason") && choice0["finish_reason"].IsString()) {
        result.finish_reason = choice0["finish_reason"].GetString();
    }

    // delta (streaming) or message (non-streaming)
    const rapidjson::Value* src = nullptr;
    if (choice0.HasMember("delta") && choice0["delta"].IsObject()) {
        src = &choice0["delta"];
    } else if (choice0.HasMember("message") && choice0["message"].IsObject()) {
        src = &choice0["message"];
    }
    if (src) {
        if (src->HasMember("content") && (*src)["content"].IsString()) {
            result.content = (*src)["content"].GetString();
        }
        if (src->HasMember("tool_calls")) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            (*src)["tool_calls"].Accept(w);
            result.tool_calls_json = buf.GetString();
        }
    }

    // usage (top-level or result.usage for FusionMind)
    auto extract_usage = [&](const rapidjson::Value& u) {
        if (u.HasMember("prompt_tokens") && u["prompt_tokens"].IsInt())
            result.prompt_tokens = u["prompt_tokens"].GetInt();
        if (u.HasMember("completion_tokens") && u["completion_tokens"].IsInt())
            result.completion_tokens = u["completion_tokens"].GetInt();
        if (u.HasMember("total_tokens") && u["total_tokens"].IsInt())
            result.total_tokens = u["total_tokens"].GetInt();
    };
    if (doc.HasMember("usage") && doc["usage"].IsObject()) {
        extract_usage(doc["usage"]);
    } else if (doc.HasMember("result") && doc["result"].IsObject()) {
        const auto& r = doc["result"];
        if (r.HasMember("usage") && r["usage"].IsObject()) {
            extract_usage(r["usage"]);
        }
    }

    return true;
}

// ============================================================
// Test fixture
// ============================================================

class OpenAIChunkParsingTest : public ::testing::Test {
protected:
    ParseResult result;

    void SetUp() override { result = ParseResult{}; }

    bool Parse(const std::string& sse_line) {
        return ParseOpenAIChunk(sse_line, result);
    }
};

// ============================================================
// Content delta tests
// ============================================================

TEST_F(OpenAIChunkParsingTest, DeltaContent) {
    ASSERT_TRUE(Parse(R"(data: {"choices":[{"delta":{"content":"你好"},"finish_reason":null,"index":0}]})"));
    EXPECT_EQ(result.content, "你好");
    EXPECT_TRUE(result.finish_reason.empty());
    EXPECT_FALSE(result.is_done);
}

TEST_F(OpenAIChunkParsingTest, MultipleContentTokens) {
    ASSERT_TRUE(Parse(R"(data: {"choices":[{"delta":{"content":"世界"},"finish_reason":null}]})"));
    EXPECT_EQ(result.content, "世界");
}

TEST_F(OpenAIChunkParsingTest, FinishReasonStop) {
    ASSERT_TRUE(Parse(R"(data: {"choices":[{"delta":{},"finish_reason":"stop","index":0}]})"));
    EXPECT_TRUE(result.content.empty());
    EXPECT_EQ(result.finish_reason, "stop");
}

TEST_F(OpenAIChunkParsingTest, FinishReasonToolCalls) {
    ASSERT_TRUE(Parse(R"(data: {"choices":[{"delta":{},"finish_reason":"tool_calls","index":0}]})"));
    EXPECT_EQ(result.finish_reason, "tool_calls");
}

// ============================================================
// Tool calls tests
// ============================================================

TEST_F(OpenAIChunkParsingTest, ToolCallsDelta) {
    const std::string sse =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
        R"("type":"function","function":{"name":"get_weather","arguments":"{\"loc\":\"北京\"}"}}]},"finish_reason":null}]})";
    ASSERT_TRUE(Parse(sse));
    EXPECT_FALSE(result.tool_calls_json.empty());
    EXPECT_NE(result.tool_calls_json.find("get_weather"), std::string::npos);
    EXPECT_NE(result.tool_calls_json.find("北京"), std::string::npos);
}

TEST_F(OpenAIChunkParsingTest, ToolCallsArgumentsAccumulation) {
    // Partial arguments chunk (simulating streaming tool_calls)
    const std::string sse1 =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"loc\":"}}]},"finish_reason":null}]})";
    ASSERT_TRUE(Parse(sse1));
    EXPECT_NE(result.tool_calls_json.find("loc"), std::string::npos);
}

// ============================================================
// [DONE] marker
// ============================================================

TEST_F(OpenAIChunkParsingTest, DoneMarker) {
    ASSERT_TRUE(Parse("data: [DONE]"));
    EXPECT_TRUE(result.is_done);
    EXPECT_TRUE(result.content.empty());
}

TEST_F(OpenAIChunkParsingTest, DoneMarkerNoPrefix) {
    ASSERT_TRUE(Parse("[DONE]"));
    EXPECT_TRUE(result.is_done);
}

// ============================================================
// FusionMind wrapped format
// ============================================================

TEST_F(OpenAIChunkParsingTest, FusionMindWrappedMessage) {
    const std::string sse =
        R"(data: {"result":{"choices":[{"message":{"content":"FusionMind回答"},)"
        R"("finish_reason":"stop","index":0}]}})";
    ASSERT_TRUE(Parse(sse));
    EXPECT_EQ(result.content, "FusionMind回答");
    EXPECT_EQ(result.finish_reason, "stop");
}

TEST_F(OpenAIChunkParsingTest, FusionMindWithUsage) {
    const std::string sse =
        R"(data: {"result":{"choices":[{"message":{"content":"带用量"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}}})";
    ASSERT_TRUE(Parse(sse));
    EXPECT_EQ(result.content, "带用量");
    EXPECT_EQ(result.prompt_tokens, 10);
    EXPECT_EQ(result.completion_tokens, 5);
    EXPECT_EQ(result.total_tokens, 15);
}

// ============================================================
// Usage extraction
// ============================================================

TEST_F(OpenAIChunkParsingTest, StandardOpenAIUsage) {
    const std::string sse =
        R"(data: {"choices":[{"message":{"content":"标准回答"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":20,"completion_tokens":8,"total_tokens":28}})";
    ASSERT_TRUE(Parse(sse));
    EXPECT_EQ(result.content, "标准回答");
    EXPECT_EQ(result.total_tokens, 28);
}

TEST_F(OpenAIChunkParsingTest, UsageOnlyInFinalChunk) {
    // First chunk: content only
    ASSERT_TRUE(Parse(R"(data: {"choices":[{"delta":{"content":"逐步"},"finish_reason":null}]})"));
    EXPECT_EQ(result.content, "逐步");
    EXPECT_EQ(result.total_tokens, 0);
    // Final chunk: finish_reason + usage
    ASSERT_TRUE(Parse(
        R"(data: {"choices":[{"delta":{},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":5,"completion_tokens":2,"total_tokens":7}})"));
    EXPECT_EQ(result.finish_reason, "stop");
    EXPECT_EQ(result.total_tokens, 7);
}

// ============================================================
// Error handling
// ============================================================

TEST_F(OpenAIChunkParsingTest, ErrorField) {
    const std::string sse =
        R"(data: {"error":{"message":"rate limit exceeded","type":"rate_limit"}})";
    ASSERT_TRUE(Parse(sse));
    EXPECT_TRUE(result.is_error);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(OpenAIChunkParsingTest, EmptyChoicesArray) {
    ASSERT_TRUE(Parse(R"(data: {"choices":[]})"));
    EXPECT_TRUE(result.content.empty());
    EXPECT_FALSE(result.is_error);
}

// ============================================================
// Edge cases
// ============================================================

TEST_F(OpenAIChunkParsingTest, WhitespaceAfterDataPrefix) {
    ASSERT_TRUE(Parse(R"(data:  {"choices":[{"delta":{"content":"空格后"},"finish_reason":null}]})"));
    EXPECT_EQ(result.content, "空格后");
}

TEST_F(OpenAIChunkParsingTest, NonStreamingMessage) {
    const std::string sse =
        R"(data: {"choices":[{"message":{"content":"非流式完整回答","role":"assistant"},)"
        R"("finish_reason":"stop","index":0}]})";
    ASSERT_TRUE(Parse(sse));
    EXPECT_EQ(result.content, "非流式完整回答");
    EXPECT_EQ(result.finish_reason, "stop");
}

TEST_F(OpenAIChunkParsingTest, NullContentIgnored) {
    ASSERT_TRUE(Parse(R"(data: {"choices":[{"delta":{"content":null},"finish_reason":null}]})"));
    EXPECT_TRUE(result.content.empty());
}

TEST_F(OpenAIChunkParsingTest, NestedToolCallsFields) {
    const std::string sse =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,)"
        R"("function":{"arguments":"{\"city\":\"上海\"}"}}]},"finish_reason":null}]})";
    ASSERT_TRUE(Parse(sse));
    EXPECT_NE(result.tool_calls_json.find("上海"), std::string::npos);
}

TEST_F(OpenAIChunkParsingTest, MalformedJsonReturnsFalse) {
    EXPECT_FALSE(Parse("data: {invalid json}"));
}

TEST_F(OpenAIChunkParsingTest, EmptySseLineReturnsFalse) {
    EXPECT_FALSE(Parse(""));
    EXPECT_FALSE(Parse("data:"));
    EXPECT_FALSE(Parse("data:  "));
}

TEST_F(OpenAIChunkParsingTest, NonObjectReturnsFalse) {
    // Non-object JSON is not parseable as a chunk — returns false
    EXPECT_FALSE(Parse(R"(data: "just a string")"));
}

// ============================================================
// Multi-chunk sequential parsing (simulating real stream)
// ============================================================

TEST_F(OpenAIChunkParsingTest, SequentialTokenParsing) {
    struct ChunkExpect {
        std::string sse;
        std::string expected_content;
        std::string expected_finish_reason;
    };

    const ChunkExpect chunks[] = {
        {R"(data: {"choices":[{"delta":{"content":"你"},"finish_reason":null}]})", "你", ""},
        {R"(data: {"choices":[{"delta":{"content":"好"},"finish_reason":null}]})", "好", ""},
        {R"(data: {"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":2,"total_tokens":4}})", "", "stop"},
    };

    int step = 0;
    for (const auto& c : chunks) {
        ParseResult r;
        ASSERT_TRUE(ParseOpenAIChunk(c.sse, r)) << "at step " << step;
        EXPECT_EQ(r.content, c.expected_content) << "at step " << step;
        EXPECT_EQ(r.finish_reason, c.expected_finish_reason) << "at step " << step;
        if (step == 2) {
            EXPECT_EQ(r.total_tokens, 4);
        }
        ++step;
    }
}

}  // namespace tests
}  // namespace runtime
}  // namespace agentflow
