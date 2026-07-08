/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 *
 * OpenAI 兼容格式转发 — IMInferServiceClient 的桥接实现。
 *
 * 本文件承载新协议（llm.chat / ForwardOpenAI*）的全部实现代码，
 * 与原 IMInferServiceClient.cpp 中的旧协议（三段式 llm.task.create → stream → destroy）隔离。
 *
 * 与旧文件的边界：
 *   - 共用 IMInferServiceClient.h 中的类声明、OpenAIChunkResult、OpenAIStreamCallback
 *   - 不共用旧文件中的 IMInferModelParams / IMInferStreamTask / StreamCallbackMessage 等旧结构
 *   - 常量（MODEL_OPENAI_URL / SSE_DATA_PREFIX / SSE_DONE_FLAG）在本文件中重新声明
 *
 * 适用方法：
 *   - ParseOpenAIChunk()     — OpenAI 格式 SSE chunk 解析
 *   - ForwardOpenAIStream()  — 流式转发（回调推事件）
 *   - ForwardOpenAIComplete()— 非流式转发（阻塞等完整响应）
 *   - GetOpenAIUrl()         — FusionMind OpenAI 端点 URL
 */

#include "sys_agent/IMInferServiceClient.h"
#include "utils/http_client.h"
#include "aisf_platform/parser/json_parser.h"
#include "utils/logger.h"
#include "framework_com/http_2_client.h"

#include <cstdint>

using namespace SYSAGENT;
using namespace AisfPlat;

namespace SysAgent {

// ==================== 常量定义 ====================

constexpr char IM_INFER_SCHEDULER[] = "IMInferScheduler";
constexpr char MODEL_INFER_PORT[] = ":27321";
constexpr char MODEL_OPENAI_URL[] = "/infermind/scheduler/v1/chat/completions";
constexpr char SSE_DATA_PREFIX[] = "data:";
constexpr char SSE_DONE_FLAG[] = "[DONE]";
constexpr char REQUEST_ID_HEADER[] = "requestId";

// ==================== GetOpenAIUrl ====================

std::string IMInferServiceClient::GetOpenAIUrl()
{
    std::string service_ip = HttpClient::GetServiceIP(IM_INFER_SCHEDULER);
    std::string url = "https://" + service_ip + MODEL_INFER_PORT + MODEL_OPENAI_URL;
    LOG_INFO_EX("[IMInferClient] GetOpenAIUrl: service_ip=%s, url=%s", service_ip.c_str(), url.c_str());
    return url;
}

// ==================== NextRequestId ====================

int64_t IMInferServiceClient::NextRequestId()
{
    return request_id_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
}

// ==================== ParseOpenAIChunk ====================

bool IMInferServiceClient::ParseOpenAIChunk(
    const std::string& sse_line,
    OpenAIChunkResult& result
)
{
    // 跳过 "data: " 前缀
    std::string_view view(sse_line);
    auto prefix_pos = view.find(SSE_DATA_PREFIX);
    if (prefix_pos != std::string::npos) {
        view.remove_prefix(prefix_pos + std::strlen(SSE_DATA_PREFIX));
    }
    // 去除前导空白
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t' || view.front() == '\r' || view.front() == '\n')) {
        view.remove_prefix(1);
    }
    if (view.empty()) {
        return false;
    }

    // 检查 [DONE] 标记
    if (view == SSE_DONE_FLAG) {
        result.is_done = true;
        return true;
    }

    // 解析 JSON
    Json::Value root;
    if (!JsonParser::Parse(std::string(view).c_str(), root)) {
        return false;
    }

    // 检查 error 字段
    if (root.isObject() && JsonParser::IsMember(root, "error")) {
        result.is_error = true;
        result.error_message = root["error"].toStyledString();
        return true;
    }

    // 定位 choices[0] — 兼容两种结构：
    //   OpenAI 标准: {"choices": [...]}
    //   FusionMind:  {"result": {"choices": [...]}}
    Json::Value* choices_ptr = nullptr;
    if (root.isObject() && JsonParser::IsMember(root, "choices") && root["choices"].isArray()) {
        choices_ptr = &root["choices"];
    } else if (root.isObject() && JsonParser::IsMember(root, "result") && root["result"].isObject()) {
        auto& r = root["result"];
        if (JsonParser::IsMember(r, "choices") && r["choices"].isArray()) {
            choices_ptr = &r["choices"];
        }
    }
    if (!choices_ptr || choices_ptr->empty()) {
        return true; // 无内容但也没错误，忽略此 chunk
    }

    const auto& choice0 = (*choices_ptr)[0];
    if (!choice0.isObject()) {
        return true;
    }

    // 提取 finish_reason
    if (JsonParser::IsMember(choice0, "finish_reason") && choice0["finish_reason"].isString()) {
        result.finish_reason = choice0["finish_reason"].asString();
    }

    // 提取 delta（流式）或 message（非流式）
    const Json::Value* content_src = nullptr;
    if (JsonParser::IsMember(choice0, "delta") && choice0["delta"].isObject()) {
        content_src = &choice0["delta"];
    } else if (JsonParser::IsMember(choice0, "message") && choice0["message"].isObject()) {
        content_src = &choice0["message"];
    }
    if (content_src) {
        // content
        if (JsonParser::IsMember(*content_src, "content") && (*content_src)["content"].isString()) {
            result.content = (*content_src)["content"].asString();
        }
        // tool_calls（保留完整 JSON 数组字符串，让调用方自己解析）
        if (JsonParser::IsMember(*content_src, "tool_calls")) {
            Json::FastWriter writer;
            result.tool_calls_json = writer.write((*content_src)["tool_calls"]);
            // 去除尾部换行
            while (!result.tool_calls_json.empty() && (result.tool_calls_json.back() == '\n' || result.tool_calls_json.back() == '\r')) {
                result.tool_calls_json.pop_back();
            }
        }
    }

    // 提取 usage（只有非流式最后的 chunk 会有）
    if (JsonParser::IsMember(root, "usage") && root["usage"].isObject()) {
        const auto& usage = root["usage"];
        if (JsonParser::IsMember(usage, "prompt_tokens") && usage["prompt_tokens"].isInt()) {
            result.prompt_tokens = usage["prompt_tokens"].asInt();
        }
        if (JsonParser::IsMember(usage, "completion_tokens") && usage["completion_tokens"].isInt()) {
            result.completion_tokens = usage["completion_tokens"].asInt();
        }
        if (JsonParser::IsMember(usage, "total_tokens") && usage["total_tokens"].isInt()) {
            result.total_tokens = usage["total_tokens"].asInt();
        }
    } else {
        // 尝试从 FusionMind 的 result.usage 取
        if (root.isObject() && JsonParser::IsMember(root, "result") && root["result"].isObject()) {
            const auto& r = root["result"];
            if (JsonParser::IsMember(r, "usage") && r["usage"].isObject()) {
                const auto& usage = r["usage"];
                if (JsonParser::IsMember(usage, "prompt_tokens") && usage["prompt_tokens"].isInt()) {
                    result.prompt_tokens = usage["prompt_tokens"].asInt();
                }
                if (JsonParser::IsMember(usage, "completion_tokens") && usage["completion_tokens"].isInt()) {
                    result.completion_tokens = usage["completion_tokens"].asInt();
                }
                if (JsonParser::IsMember(usage, "total_tokens") && usage["total_tokens"].isInt()) {
                    result.total_tokens = usage["total_tokens"].asInt();
                }
            }
        }
    }

    return true;
}

// ==================== ForwardOpenAIStream ====================

bool IMInferServiceClient::ForwardOpenAIStream(
    const std::string& request_body_json,
    const OpenAIStreamCallback& on_event,
    uint32_t& error_code
)
{
    LOG_INFO_EX("[IMInferClient] ForwardOpenAIStream ENTER: body_len=%zu", request_body_json.size());

    const int64_t request_id = NextRequestId();
    LOG_INFO_EX("[IMInferClient] ForwardOpenAIStream: requestId=%lld", static_cast<long long>(request_id));
    LOG_INFO_EX("[IMInferClient] ForwardOpenAIStream REQ body: %s", request_body_json.c_str());

    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetMethod(CMFrm::COM::POST);
    request->SetReqUrl(GetOpenAIUrl());
    request->AddBody(request_body_json);
    request->AddJSONContentTypeHeader();
    request->AddHeader(REQUEST_ID_HEADER, std::to_string(request_id));
    request->SetTimeoutSecond(6000);

    bool sendOk = HttpClient::Http2SendStreamRequest(request,
        [this, on_event](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
            std::string body = resp->GetStreamResponseBody();
            bool is_last = resp->IsLastStreamData();

            LOG_INFO_EX("[IMInferClient] ForwardOpenAIStream RESP chunk: body=%s, is_last=%d",
                body.c_str(), static_cast<int>(is_last));

            OpenAIChunkResult result;
            if (ParseOpenAIChunk(body, result)) {
                if (on_event) {
                    on_event(result);
                }
            } else {
                LOG_WARN("[IMInferClient] ForwardOpenAIStream: failed to parse chunk, body=%s",
                    body.substr(0, 200).c_str());
            }

            if (is_last && result.is_done) {
                LOG_INFO_EX("[IMInferClient] ForwardOpenAIStream: stream done");
            }
        }, nullptr);

    if (!sendOk) {
        error_code = 2;
        LOG_ERR_EX("[IMInferClient] ForwardOpenAIStream FAILED: HTTP send failed");
        return false;
    }

    error_code = 0;
    LOG_INFO_EX("[IMInferClient] ForwardOpenAIStream SUCCESS");
    return true;
}

// ==================== ForwardOpenAIComplete ====================

OpenAIChunkResult IMInferServiceClient::ForwardOpenAIComplete(
    const std::string& request_body_json,
    int timeout_seconds
)
{
    LOG_INFO_EX("[IMInferClient] ForwardOpenAIComplete ENTER: body_len=%zu, timeout=%ds",
        request_body_json.size(), timeout_seconds);

    const int64_t request_id = NextRequestId();
    LOG_INFO_EX("[IMInferClient] ForwardOpenAIComplete: requestId=%lld", static_cast<long long>(request_id));
    LOG_INFO_EX("[IMInferClient] ForwardOpenAIComplete REQ body: %s", request_body_json.c_str());

    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetMethod(CMFrm::COM::POST);
    request->SetReqUrl(GetOpenAIUrl());
    request->AddBody(request_body_json);
    request->AddJSONContentTypeHeader();
    request->AddHeader(REQUEST_ID_HEADER, std::to_string(request_id));
    request->SetTimeoutSecond(timeout_seconds);

    std::string response;
    if (!HttpClient::Htt2SendRequest(request, response)) {
        LOG_ERR_EX("[IMInferClient] ForwardOpenAIComplete FAILED: HTTP send failed");
        OpenAIChunkResult err;
        err.is_error = true;
        err.error_message = "HTTP request failed";
        return err;
    }

    LOG_INFO_EX("[IMInferClient] ForwardOpenAIComplete RESP raw: %s", response.c_str());

    // 解析完整 SSE 响应，取最后一个非空结果（含 usage）
    OpenAIChunkResult merged;
    size_t pos = 0;
    size_t data_prefix_len = std::strlen(SSE_DATA_PREFIX);
    while ((pos = response.find(SSE_DATA_PREFIX, pos)) != std::string::npos) {
        size_t next_pos = response.find(SSE_DATA_PREFIX, pos + data_prefix_len);
        std::string chunk = (next_pos == std::string::npos)
            ? response.substr(pos)
            : response.substr(pos, next_pos - pos);

        OpenAIChunkResult chunk_result;
        if (ParseOpenAIChunk(chunk, chunk_result)) {
            if (!chunk_result.content.empty()) {
                merged.content = chunk_result.content;
            }
            if (!chunk_result.finish_reason.empty()) {
                merged.finish_reason = chunk_result.finish_reason;
            }
            if (!chunk_result.tool_calls_json.empty()) {
                merged.tool_calls_json = chunk_result.tool_calls_json;
            }
            if (chunk_result.total_tokens > 0) {
                merged.prompt_tokens = chunk_result.prompt_tokens;
                merged.completion_tokens = chunk_result.completion_tokens;
                merged.total_tokens = chunk_result.total_tokens;
            }
            if (chunk_result.is_error) {
                merged.is_error = true;
                merged.error_message = chunk_result.error_message;
            }
        }
        pos = (next_pos == std::string::npos) ? std::string::npos : next_pos;
    }

    LOG_INFO_EX("[IMInferClient] ForwardOpenAIComplete SUCCESS: content_len=%zu, finish_reason=%s, tokens=%d/%d/%d",
        merged.content.size(), merged.finish_reason.c_str(),
        merged.prompt_tokens, merged.completion_tokens, merged.total_tokens);

    return merged;
}

} // namespace SysAgent
