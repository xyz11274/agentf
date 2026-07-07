#!/usr/bin/env python3
"""
FusionMind Mock Server — 本地调试用 HTTP Mock / Proxy

模拟 FusionMind 推理引擎的 HTTP API，支持两种模式：
  - mock 模式：返回预设的 mock 响应（echo / fixed / template / tool_calls）
  - proxy 模式：透传请求到真实的 LLM API（需设置 --upstream-url）

用法：
  # mock 模式（默认 echo）
  python3 frontend/mocks/mocks_fusionmind/server.py --port 8085

  # proxy 模式（透传到真实 LLM）
  python3 frontend/mocks/mocks_fusionmind/server.py --port 8085 --proxy \
      --upstream-url https://api.openai.com/v1 --api-key sk-xxx

  # 流式 mock
  python3 frontend/mocks/mocks_fusionmind/server.py --mode fixed --response "你好！"
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import random
import re
import time
import uuid
from collections.abc import AsyncGenerator
from typing import Any

import httpx
import uvicorn
from fastapi import FastAPI, Request, Response
from fastapi.responses import StreamingResponse

logging.basicConfig(
    level=logging.DEBUG,
    format="[MOCK_FM] %(asctime)s.%(msecs)03d %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("fusionmind_mock")

app = FastAPI(title="FusionMind Mock Server")

# ── 全局状态 ──────────────────────────────────────────────────────────

_config: dict[str, Any] = {}
_stats = {"total_requests": 0, "active_connections": 0}


# ── 配置 ──────────────────────────────────────────────────────────────


def load_config(args: argparse.Namespace) -> dict[str, Any]:
    """从命令行参数加载配置。"""
    return {
        "mode": args.mode,
        "response": args.response,
        "token_delay": args.token_delay,
        "completion_delay": args.completion_delay,
        "error_rate": args.error_rate,
        "proxy": args.proxy,
        "upstream_url": args.upstream_url,
        "api_key": args.api_key,
        "model": args.model,
        "mock_usage": args.mock_usage,
        "use_result_wrapper": args.use_result_wrapper,
    }


def configure(args: argparse.Namespace) -> None:
    """启动前配置全局状态。"""
    global _config
    _config = load_config(args)


# ── 工具函数 ──────────────────────────────────────────────────────────


def _model_dump_json(obj: Any) -> str:
    """安全序列化（handle non-serializable）。"""
    return json.dumps(obj, ensure_ascii=False, default=str)


def _parse_messages(messages: list[dict]) -> tuple[str, str]:
    """从 messages 中提取 system prompt 和最后一条 user 消息。"""
    system_prompt = ""
    last_user = ""
    for msg in messages:
        role = msg.get("role", "")
        content = msg.get("content", "")
        if role == "system" and isinstance(content, str):
            system_prompt = content
        elif role == "user" and isinstance(content, str):
            last_user = content
    return system_prompt, last_user


def _build_usage(prompt_tokens: int, completion_tokens: int) -> dict:
    return {
        "prompt_tokens": prompt_tokens,
        "completion_tokens": completion_tokens,
        "total_tokens": prompt_tokens + completion_tokens,
    }


def _random_usage() -> dict:
    return _build_usage(
        prompt_tokens=random.randint(50, 200),
        completion_tokens=random.randint(10, 100),
    )


# ── Mock 响应生成 ─────────────────────────────────────────────────────


def _mock_answer(system_prompt: str, last_user: str) -> str:
    """根据配置生成 mock 回答。"""
    mode = _config.get("mode", "echo")

    if mode == "echo":
        return f"你说了: {last_user}" if last_user else "（空消息）"

    if mode == "fixed":
        return _config.get("response", "这是一条 mock 回复。")

    if mode == "template":
        templates = _config.get("templates", [
            {"pattern": ".*天气.*", "response": "北京今天25度，晴"},
            {"pattern": ".*你好.*", "response": "你好！我是穿搭顾问。"},
            {"pattern": ".*",       "response": "收到你的查询（根据意图规划中…）"},
        ])
        for t in templates:
            if re.search(t["pattern"], last_user):
                return t["response"]
        return templates[-1]["response"]

    if mode == "tool_calls":
        return ""  # tool_calls 模式不走文本回答

    # 默认
    return f"收到消息（len={len(last_user)}），prompt={len(system_prompt)}"


def _mock_tool_calls() -> list[dict]:
    """生成 mock tool_calls 响应。"""
    tools_cfg = _config.get("tools", [
        {"name": "get_weather", "arguments": {"city": "北京"}, "output": "北京今天25度，晴"},
    ])
    tool = tools_cfg[0]
    return [{
        "id": f"call_mock_{uuid.uuid4().hex[:8]}",
        "type": "function",
        "function": {
            "name": tool["name"],
            "arguments": _model_dump_json(tool.get("arguments", {})),
        },
    }]


# ── SSE 流式生成 ──────────────────────────────────────────────────────


async def _mock_stream_generator(request: Request) -> AsyncGenerator[str, None]:
    """以 SSE 格式逐 token 推送 mock 回答。"""
    body = await request.json()
    messages = body.get("messages", [])
    system_prompt, last_user = _parse_messages(messages)
    tools = body.get("tools", [])

    answer = _mock_answer(system_prompt, last_user)
    finish_reason = "stop"
    delay = _config.get("token_delay", 0.02)
    err_rate = _config.get("error_rate", 0.0)
    use_result = _config.get("use_result_wrapper", False)

    stats_active("incr")

    try:
        is_tool_call = _config.get("mode") == "tool_calls"

        if is_tool_call and tools:
            # 模拟 tool_call 流式推送
            tool_calls = _mock_tool_calls()
            finish_reason = "tool_calls"
            for tc in tool_calls:
                # 帧 1: id + name
                delta = {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [{
                        "index": 0,
                        "id": tc["id"],
                        "type": tc["type"],
                        "function": {"name": tc["function"]["name"], "arguments": ""},
                    }],
                }
                yield _sse_line(delta, None, use_result)
                if delay > 0:
                    await _async_sleep(delay)

                # 帧 2-N: 逐字符 arguments
                args_str = tc["function"]["arguments"]
                chunk_size = max(1, len(args_str) // 5)
                for i in range(0, len(args_str), chunk_size):
                    chunk = args_str[i:i + chunk_size]
                    delta = {
                        "tool_calls": [{"index": 0, "function": {"arguments": chunk}}],
                    }
                    yield _sse_line(delta, None, use_result)
                    if delay > 0:
                        await _async_sleep(delay)

        elif answer:
            # 逐 token 推送
            for i, ch in enumerate(answer):
                if random.random() < err_rate:
                    yield _sse_error_line("500", "mock error", use_result)
                    return

                delta = {"content": ch}
                yield _sse_line(delta, None, use_result)
                if delay > 0:
                    await _async_sleep(delay)

        # 最终帧
        usage = _random_usage() if _config.get("mock_usage", True) else None
        yield _sse_final_line(finish_reason, usage, use_result)
        yield "data: [DONE]\n\n"

    finally:
        stats_active("decr")


async def _proxy_stream_generator(request: Request) -> AsyncGenerator[str, None]:
    """透传流式请求到上游 LLM，并逐帧转发 SSE。"""
    upstream = _config["upstream_url"].rstrip("/")
    api_key = _config.get("api_key", "")
    model = _config.get("model", "")

    body = await request.json()
    if model:
        body["model"] = model

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    }

    stats_active("incr")
    try:
        async with httpx.AsyncClient(timeout=300) as client:
            async with client.stream(
                "POST", f"{upstream}/chat/completions",
                json=body, headers=headers,
            ) as resp:
                async for line in resp.aiter_lines():
                    if line.startswith("data: "):
                        yield line + "\n"
    except Exception as e:
        logger.error("Proxy error: %s", e)
        yield _sse_error_line("502", f"Proxy error: {e}", _config.get("use_result_wrapper", False))
        yield "data: [DONE]\n\n"
    finally:
        stats_active("decr")


# ── SSE 格式化 ────────────────────────────────────────────────────────


def _sse_line(delta: dict, finish_reason: str | None, use_result: bool) -> str:
    chunk = {
        "choices": [{
            "delta": delta,
            "index": 0,
        }],
    }
    if finish_reason:
        chunk["choices"][0]["finish_reason"] = finish_reason
    if use_result:
        chunk = {"result": chunk}
    return f"data: {_model_dump_json(chunk)}\n\n"


def _sse_final_line(finish_reason: str, usage: dict | None, use_result: bool) -> str:
    chunk = {
        "choices": [{
            "delta": {},
            "finish_reason": finish_reason,
            "index": 0,
        }],
    }
    if usage:
        chunk["usage"] = usage
    if use_result:
        chunk = {"result": chunk}
    return f"data: {_model_dump_json(chunk)}\n\n"


def _sse_error_line(error_code: str, message: str, use_result: bool) -> str:
    err = {"error": {"code": error_code, "message": message}}
    if use_result:
        err = {"result": err}
    return f"data: {_model_dump_json(err)}\n\n"


# ── 帮助函数 ──────────────────────────────────────────────────────────


def stats_active(action: str) -> None:
    if action == "incr":
        _stats["active_connections"] += 1
    elif action == "decr":
        _stats["active_connections"] = max(0, _stats["active_connections"] - 1)
    _stats["total_requests"] += 1


async def _async_sleep(seconds: float) -> None:
    """异步 sleep，避免阻塞事件循环。"""
    await _async_sleep._coro(seconds)


_async_sleep._coro = __import__("asyncio").sleep


# ── 非流式响应 ────────────────────────────────────────────────────────


async def _handle_non_stream(body: dict) -> dict:
    """非流式请求返回完整 JSON 响应。"""
    messages = body.get("messages", [])
    tools = body.get("tools", [])
    system_prompt, last_user = _parse_messages(messages)

    delay = _config.get("completion_delay", 0)
    if delay > 0:
        await _async_sleep(delay)

    is_tool_call = _config.get("mode") == "tool_calls"
    content = None if (is_tool_call and tools) else _mock_answer(system_prompt, last_user)

    message: dict[str, Any] = {"role": "assistant", "content": content}
    finish_reason = "stop"

    if is_tool_call and tools:
        message["tool_calls"] = _mock_tool_calls()
        finish_reason = "tool_calls"

    choice = {
        "index": 0,
        "message": message,
        "finish_reason": finish_reason,
    }

    response = {
        "id": f"chatcmpl-mock-{uuid.uuid4().hex[:12]}",
        "object": "chat.completion",
        "created": int(time.time()),
        "model": _config.get("model", "mock-model"),
        "choices": [choice],
    }
    if _config.get("mock_usage", True):
        response["usage"] = _random_usage()
    return response


# ── 请求处理 ──────────────────────────────────────────────────────────


async def _handle_completions(request: Request) -> Response:
    """通用 completions 处理器。"""
    body = await request.json()
    stream = body.get("stream", False)
    is_proxy = _config.get("proxy", False)

    if stream:
        if is_proxy:
            generator = _proxy_stream_generator(request)
        else:
            generator = _mock_stream_generator(request)
        return StreamingResponse(
            generator,
            media_type="text/event-stream",
            headers={
                "Cache-Control": "no-cache",
                "Connection": "keep-alive",
                "X-Accel-Buffering": "no",
            },
        )
    else:
        if is_proxy:
            return await _proxy_non_stream(body)
        data = await _handle_non_stream(body)
        return Response(
            content=_model_dump_json(data),
            media_type="application/json",
        )


async def _proxy_non_stream(body: dict) -> Response:
    """非流式透传到上游。"""
    upstream = _config["upstream_url"].rstrip("/")
    api_key = _config.get("api_key", "")
    model = _config.get("model", "")
    if model:
        body["model"] = model

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    }

    try:
        async with httpx.AsyncClient(timeout=300) as client:
            resp = await client.post(
                f"{upstream}/chat/completions",
                json=body, headers=headers,
            )
            return Response(
                content=resp.text,
                status_code=resp.status_code,
                media_type="application/json",
            )
    except Exception as e:
        logger.error("Proxy error: %s", e)
        return Response(
            content=_model_dump_json({
                "error": {"code": "502", "message": f"Proxy error: {e}"},
            }),
            status_code=502,
            media_type="application/json",
        )


# ── 路由 ──────────────────────────────────────────────────────────────


@app.post("/infermind/llm/v1/chat/openai/completions")
async def openai_completions(request: Request) -> Response:
    """OpenAI 标准格式接口。"""
    logger.info("→ openai_completions")
    return await _handle_completions(request)


@app.post("/infermind/llm/v1/chat/completions")
async def legacy_completions(request: Request) -> Response:
    """FusionMind 遗留格式接口（当前 SysAgent 使用）。"""
    logger.info("→ legacy_completions")
    return await _handle_completions(request)


@app.get("/health")
async def health() -> dict:
    return {
        "status": "ok",
        "server": "FusionMind Mock Server",
        "version": "1.0.0",
        "config": {k: v for k, v in _config.items() if k != "api_key"},
        "stats": _stats,
    }


# ── CLI ───────────────────────────────────────────────────────────────


def main() -> None:
    parser = argparse.ArgumentParser(description="FusionMind Mock Server")
    parser.add_argument("--port", type=int, default=8085, help="监听端口（默认 8085）")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址")
    parser.add_argument("--mode", default="echo",
                        choices=["echo", "fixed", "template", "tool_calls"],
                        help="响应模式（仅 mock 模式生效）")
    parser.add_argument("--response", default="", help="fixed 模式下的响应文本")
    parser.add_argument("--model", default="aisf/deepseek-v4-flash", help="模型名")
    parser.add_argument("--token-delay", type=float, default=0.02, help="每 token 延迟秒数")
    parser.add_argument("--completion-delay", type=float, default=0, help="非流式响应延迟秒数")
    parser.add_argument("--error-rate", type=float, default=0.0, help="错误注入概率 0-1")
    parser.add_argument("--no-mock-usage", dest="mock_usage", action="store_false",
                        help="不模拟 usage 统计")
    parser.add_argument("--use-result-wrapper", action="store_true",
                        help='使用 FusionMind "result" 包装格式')
    parser.add_argument("--proxy", action="store_true", help="启用 proxy 模式（透传上游）")
    parser.add_argument("--upstream-url", default="",
                        help="上游 LLM API base URL（需 --proxy）")
    parser.add_argument("--api-key", default="", help="上游 LLM API Key（配合 --proxy）")
    parser.add_argument("--log-level", default="info",
                        choices=["debug", "info", "warning", "error"],
                        help="日志级别")

    args = parser.parse_args()
    logging.getLogger("fusionmind_mock").setLevel(getattr(logging, args.log_level.upper()))

    configure(args)

    if args.proxy and not args.upstream_url:
        # 尝试从环境变量读取
        args.upstream_url = os.environ.get("AF_LLM_BASE_URL", "https://api.openai.com/v1")
        args.api_key = args.api_key or os.environ.get("AF_LLM_API_KEY", "")
        logger.info("Proxy mode: upstream=%s (from env/args)", args.upstream_url)

    mode_label = "PROXY" if args.proxy else "MOCK"
    logger.info("=" * 60)
    logger.info("FusionMind Mock Server 启动 (%s 模式)", mode_label)
    logger.info("  Listen: http://%s:%d", args.host, args.port)
    logger.info("  Mode:   %s", args.mode if not args.proxy else f"proxy → {args.upstream_url}")
    if not args.proxy:
        logger.info("  Token delay: %.3fs  Error rate: %.1f%%",
                    args.token_delay, args.error_rate * 100)
    logger.info("  Endpoints:")
    logger.info("    POST /infermind/llm/v1/chat/openai/completions")
    logger.info("    POST /infermind/llm/v1/chat/completions")
    logger.info("    GET  /health")
    logger.info("=" * 60)

    uvicorn.run(app, host=args.host, port=args.port, log_level=args.log_level)


if __name__ == "__main__":
    main()
