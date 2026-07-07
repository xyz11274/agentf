/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * 
 * IMInferService REST接口客户端封装类
 * 
 * 功能说明：
 *   封装IMInferService的REST接口调用。
 *   支持 OpenAI 兼容格式透传（ForwardOpenAIStream / ForwardOpenAIComplete）。
 * 
 * 实现分布：
 *   - 新协议（OpenAI 兼容）：IMInferServiceClient.OpenAI.cpp
 *   - 旧协议（已废弃，准备删除）：IMInferServiceClient.cpp
 */
#ifndef SYSAGENT_IMINFERSERVICECLIENT_H
#define SYSAGENT_IMINFERSERVICECLIENT_H

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include "framework_com/http_2_client.h"

namespace SysAgent {

/**
 * OpenAI 兼容解析结果
 * 
 * 对应 OpenAI Chat Completions 格式中的单个 SSE chunk 解析结果。
 * 支持 delta.content / delta.tool_calls / finish_reason / usage。
 */
struct OpenAIChunkResult {
    std::string content;                          // delta.content
    std::string finish_reason;                    // "stop" | "length" | "tool_calls" | ""
    std::string tool_calls_json;                  // delta.tool_calls JSON 数组字符串
    int prompt_tokens{0};
    int completion_tokens{0};
    int total_tokens{0};
    bool is_error{false};
    std::string error_message;
    bool is_done{false};                          // [DONE] 标记
};

/**
 * OpenAI 流式回调函数类型
 * 
 * 参数为解析后的 OpenAIChunkResult，包含 content / finish_reason / tool_calls / usage 等完整信息。
 */
using OpenAIStreamCallback = std::function<void(const OpenAIChunkResult& result)>;

/**
 * IMInferService REST接口客户端类
 * 
 * 封装模型调用的REST接口，提供 OpenAI 兼容格式透传能力。
 * 
 * 使用示例（新协议，推荐）：
 *   uint32_t error_code = 0;
 *   IMInferServiceClient::Instance()->ForwardOpenAIStream(
 *       "{\"messages\":[...]}",
 *       [](const OpenAIChunkResult& result) {
 *           // 处理流式事件
 *       },
 *       error_code
 *   );
 */
class IMInferServiceClient {
public:
    /**
     * 获取单例实例
     * 
     * @return 单例实例指针
     */
    static std::shared_ptr<IMInferServiceClient> Instance();

    // ────────────────────────────────────────────────────────────
    // OpenAI 兼容格式透传（新协议，llm.chat 路径）
    // ────────────────────────────────────────────────────────────

    /**
     * 流式转发：直接接收完整 OpenAI 格式请求体，透传给 FusionMind 的 OpenAI 端点。
     * 
     * 内部自动管理 HTTP 连接和 SSE 解析，通过 on_event 回调推送解析结果。
     * 回调包含 content / finish_reason / tool_calls_json / usage 等完整字段。
     * 
     * @param request_body_json  完整的 OpenAI Chat Completions 请求体 JSON
     * @param on_event           流式事件回调（每次 SSE chunk 触发一次）
     * @param error_code         输出参数，错误码（0 表示成功）
     * @return 是否成功发起请求
     */
    bool ForwardOpenAIStream(
        const std::string& request_body_json,
        const OpenAIStreamCallback& on_event,
        uint32_t& error_code
    );

    /**
     * 同步转发：直接接收完整 OpenAI 格式请求体，阻塞等待完整响应。
     * 
     * @param request_body_json  完整的 OpenAI Chat Completions 请求体 JSON
     * @param timeout_seconds    超时时间（秒）
     * @return 解析后的 OpenAIChunkResult（含 content / tool_calls_json / usage 等）
     */
    OpenAIChunkResult ForwardOpenAIComplete(
        const std::string& request_body_json,
        int timeout_seconds = 60
    );

    ~IMInferServiceClient();

private:
    IMInferServiceClient();
    IMInferServiceClient(const IMInferServiceClient&) = delete;
    IMInferServiceClient& operator=(const IMInferServiceClient&) = delete;

    /**
     * 获取FusionMind OpenAI 兼容端点 URL
     * 
     * @return OpenAI 兼容端点完整 URL
     */
    std::string GetOpenAIUrl();

    /**
     * 解析 OpenAI 格式的单个 SSE chunk。
     * 
     * 支持两种响应结构：
     *   - OpenAI 标准流式: {"choices":[{"delta":{"content":"..."},"finish_reason":null}]}
     *   - FusionMind 包装: {"result":{"choices":[{"message":{"content":"..."},"finish_reason":"stop"}]}}
     * 
     * @param sse_line  完整的 SSE 行（含 "data: " 前缀或裸 JSON）
     * @param result    输出参数，解析结果
     * @return 是否成功解析
     */
    bool ParseOpenAIChunk(
        const std::string& sse_line,
        OpenAIChunkResult& result
    );

    // 单例相关静态成员
    static std::shared_ptr<IMInferServiceClient> instance_;
    static std::once_flag once_flag_;
};

} // namespace SysAgent

#endif // SYSAGENT_IMINFERSERVICECLIENT_H
