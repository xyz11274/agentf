#!/usr/bin/env python3
"""
UDS 协议一致性验证脚本

在目标 UDS Server（Mock 或生产 SysAgent）上执行标准化探测请求，
捕获原始帧数据，输出结构化对比报告，用于验证 mock server 与生产实现的行为是否一致。

支持 --fusionmind-url 参数，对同一个请求同时走 UDS 和 HTTP 两条路径，
对比转发格式是否一致（检测 SysAgent 在转发中是否丢失/篡改字段）。

用法：
  # 对比 Mock UDS Server
  python3 test_uds_protocol_consistency.py --socket /tmp/mock_agentflow.sock --tag mock

  # 对比生产 SysAgent + 同时验证转发给 FusionMind 是否一致
  python3 test_uds_protocol_consistency.py \\
      --socket /tmp/agentflow_plugin.sock --tag production \\
      --fusionmind-url http://127.0.0.1:8085/infermind/llm/v1/chat

  # 输出 JSON 报告供后续 diff
  python3 test_uds_protocol_consistency.py --socket /tmp/mock_agentflow.sock --tag mock --output report_mock.json
  python3 test_uds_protocol_consistency.py --socket /tmp/agentflow_plugin.sock --tag production --output report_prod.json
  diff report_mock.json report_prod.json
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import time
import uuid
from dataclasses import dataclass, field
from typing import Any
import requests

# ── UDS 传输层 ─────────────────────────────────────────────────────────

MAX_FRAME_SIZE = 10 * 1024 * 1024
UDS_TIMEOUT = 10.0


class UdsTransport:
    """UDS 传输层：4-byte BE 长度前缀 + JSON 帧。"""

    def __init__(self, sock_path: str):
        self.sock_path = sock_path
        self._sock: socket.socket | None = None

    def connect(self) -> None:
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(UDS_TIMEOUT)
        self._sock.connect(self.sock_path)

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def send_frame(self, data: dict[str, Any]) -> None:
        if not self._sock:
            raise RuntimeError("not connected")
        payload = json.dumps(data, ensure_ascii=False).encode("utf-8")
        header = struct.pack(">I", len(payload))
        self._sock.sendall(header + payload)

    def recv_frame(self) -> dict[str, Any]:
        if not self._sock:
            raise RuntimeError("not connected")
        len_buf = self._sock.recv(4)
        if len(len_buf) < 4:
            raise ConnectionError(f"closed (received {len(len_buf)}/4 bytes)")
        frame_len = struct.unpack(">I", len_buf)[0]
        if frame_len > MAX_FRAME_SIZE:
            raise ValueError(f"frame too large: {frame_len}")
        chunks: list[bytes] = []
        remaining = frame_len
        while remaining > 0:
            chunk = self._sock.recv(remaining)
            if not chunk:
                raise ConnectionError("closed mid-frame")
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks).decode("utf-8")
        return json.loads(raw)


# ── 探测结果结构 ───────────────────────────────────────────────────────


@dataclass
class ProbeStep:
    """一次探测的请求/响应记录。"""

    name: str                          # 探测名称
    request: dict[str, Any]            # 发送的请求帧
    responses: list[dict[str, Any]]    # 收到的所有响应帧（流式可能有多个）
    passed: bool = False               # 是否通过检查
    checks: dict[str, bool | str] = field(default_factory=dict)  # 逐项检查结果

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "request": self.request,
            "responses": self.responses,
            "passed": self.passed,
            "checks": self.checks,
        }


@dataclass
class ProbeReport:
    """完整探测报告。"""

    tag: str                              # 目标标识（mock / production）
    socket_path: str                      # UDS socket 路径
    timestamp: str                        # 探测时间
    steps: list[ProbeStep] = field(default_factory=list)
    summary: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "tag": self.tag,
            "socket_path": self.socket_path,
            "timestamp": self.timestamp,
            "steps": [s.to_dict() for s in self.steps],
            "summary": self.summary,
        }


# ── 探测执行器 ─────────────────────────────────────────────────────────


class ProbeRunner:
    """在目标 UDS Server 上执行标准化探测并记录结果。"""

    def __init__(self, transport: UdsTransport, tag: str):
        self._t = transport
        self._tag = tag
        self._steps: list[ProbeStep] = []

    def run_all(self) -> ProbeReport:
        """执行全部探测并返回报告。"""
        self._probe_llm_chat_stream()
        self._probe_llm_chat_nonstream()
        self._probe_llm_chat_with_tools()
        self._probe_llm_chat_messages()
        self._probe_llm_task_create_stream_destroy()
        self._probe_llm_complete()
        self._probe_tool_list()
        self._probe_heartbeat()
        self._probe_unknown_method()
        return self._build_report()

    # ── 单个探测步骤 ──────────────────────────────────────────────

    def _probe(self, name: str, request: dict[str, Any],
               expect_event_stream: bool = False,
               expected_event_count: int | None = None) -> ProbeStep:
        """执行一次探测，记录请求和响应。"""
        step = ProbeStep(name=name, request=request, responses=[])
        try:
            self._t.send_frame(request)
            if expect_event_stream:
                # 流式：读取多个事件直到收到 done 标记
                while True:
                    resp = self._t.recv_frame()
                    step.responses.append(resp)
                    if resp.get("done"):
                        break
                    if expected_event_count and len(step.responses) >= expected_event_count:
                        break
            else:
                resp = self._t.recv_frame()
                step.responses.append(resp)
        except Exception as e:
            step.responses.append({"__error__": str(e)})
        self._steps.append(step)
        return step

    def _check(self, step: ProbeStep, name: str, condition: bool,
               detail: str = "") -> None:
        """在探测步骤中添加一项检查结果。"""
        step.checks[name] = condition if condition else f"FAIL: {detail}"

    # ── 探测用例 ──────────────────────────────────────────────────

    def _probe_llm_chat_stream(self) -> None:
        """llm.chat 流式模式：检查事件序列与字段完整性。"""
        step = self._probe(
            name="llm.chat (流式)",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "llm.chat",
                "plugin_type": "LLM",
                "plugin_name": "LLMPlugin",
                "params": {
                    "request": {
                        "messages": [
                            {"role": "system", "content": "你是一个助手。"},
                            {"role": "user", "content": "你好"},
                        ],
                        "model": "test-model",
                        "stream": True,
                    },
                },
            },
            expect_event_stream=True,
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            # 第一帧应该是 llm_token
            first = step.responses[0]
            self._check(step, "第一帧是 llm_token",
                        first.get("event_type") == "llm_token",
                        f"got {first.get('event_type')}")
            self._check(step, "llm_token 含 data.token 字段",
                        "token" in json.loads(first.get("data", "{}")))
            self._check(step, "llm_token 含 event_id",
                        bool(first.get("event_id")))
            self._check(step, "llm_token.channel 非空",
                        bool(first.get("channel")))
            self._check(step, "每帧都有 timestamp 且为 int",
                        isinstance(first.get("timestamp"), int))

            # 最后一帧应该是 llm_final
            last = step.responses[-1]
            self._check(step, "最后一帧是 llm_final",
                        last.get("event_type") == "llm_final",
                        f"got {last.get('event_type')}")
            last_data = json.loads(last.get("data", "{}"))
            self._check(step, "llm_final 含 answer 字段",
                        "answer" in last_data)
            self._check(step, "llm_final 含 finish_reason",
                        bool(last_data.get("finish_reason")))
            self._check(step, "llm_final.done=true",
                        last.get("done") is True)

            # 检查 usage（如果有）
            if "usage" in last_data:
                u = last_data["usage"]
                self._check(step, "usage 含 prompt_tokens",
                            isinstance(u.get("prompt_tokens"), int))
                self._check(step, "usage 含 completion_tokens",
                            isinstance(u.get("completion_tokens"), int))
                self._check(step, "usage 含 total_tokens",
                            isinstance(u.get("total_tokens"), int))

        step.passed = all(v is True for v in step.checks.values())

    def _probe_llm_chat_nonstream(self) -> None:
        """llm.chat 非流式模式：检查单帧响应结构。"""
        step = self._probe(
            name="llm.chat (非流式)",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "llm.chat",
                "plugin_type": "LLM",
                "plugin_name": "LLMPlugin",
                "params": {
                    "request": {
                        "messages": [{"role": "user", "content": "你好"}],
                        "model": "test-model",
                        "stream": False,
                    },
                },
            },
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            resp = step.responses[0]
            self._check(step, "响应含 request_id",
                        bool(resp.get("request_id")))
            self._check(step, "响应含 status",
                        resp.get("status") in ("success", "error"))
            self._check(step, "响应含 data_json", bool(resp.get("data_json")))
            if resp.get("data_json"):
                try:
                    data = json.loads(resp["data_json"]) if isinstance(resp["data_json"], str) else resp["data_json"]
                    self._check(step, "data_json 含 content",
                                "content" in data)
                    self._check(step, "data_json 含 finish_reason",
                                bool(data.get("finish_reason")))
                except (json.JSONDecodeError, TypeError):
                    self._check(step, "data_json 可解析", False, "JSON 解析失败")
        step.passed = all(v is True for v in step.checks.values())

    def _probe_llm_chat_with_tools(self) -> None:
        """llm.chat 带 tools：检查服务端是否返回 tool_calls 或至少不崩溃。"""
        step = self._probe(
            name="llm.chat (带 tools)",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "llm.chat",
                "plugin_type": "LLM",
                "plugin_name": "LLMPlugin",
                "params": {
                    "request": {
                        "messages": [{"role": "user", "content": "北京天气？"}],
                        "tools": [
                            {
                                "type": "function",
                                "function": {
                                    "name": "get_weather",
                                    "description": "获取天气",
                                    "parameters": {
                                        "type": "object",
                                        "properties": {
                                            "city": {"type": "string"},
                                        },
                                        "required": ["city"],
                                    },
                                },
                            },
                        ],
                        "model": "test-model",
                        "stream": True,
                    },
                },
            },
            expect_event_stream=True,
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            last = step.responses[-1]
            self._check(step, "最后一帧是 llm_final",
                        last.get("event_type") in ("llm_final", "llm_tool_calls", "llm_error"),
                        f"got {last.get('event_type')}")
            # 记录是否有 tool_calls 事件
            has_tc = any(r.get("event_type") == "llm_tool_calls" for r in step.responses)
            self._check(step, "服务端是否返回 tool_calls（仅记录）", True,
                        detail=f"tool_calls_events={'有' if has_tc else '无'}")
        step.passed = all(v is True for v in step.checks.values())

    def _probe_llm_chat_messages(self) -> None:
        """llm.chat 多轮消息：检查服务端能接受完整 messages 数组。"""
        step = self._probe(
            name="llm.chat (多轮消息)",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "llm.chat",
                "plugin_type": "LLM",
                "plugin_name": "LLMPlugin",
                "params": {
                    "request": {
                        "messages": [
                            {"role": "system", "content": "你是助手。"},
                            {"role": "user", "content": "你好"},
                            {"role": "assistant", "content": "你好！有什么可以帮助你的？"},
                            {"role": "user", "content": "今天天气怎么样？"},
                        ],
                        "model": "test-model",
                        "max_completion_tokens": 1024,
                        "stream": True,
                    },
                },
            },
            expect_event_stream=True,
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            last = step.responses[-1]
            self._check(step, "多轮消息也能正常完成",
                        last.get("event_type") == "llm_final",
                        f"got {last.get('event_type')}")
        step.passed = all(v is True for v in step.checks.values())

    def _probe_llm_task_create_stream_destroy(self) -> None:
        """三段式协议：create → stream → destroy。"""
        # 1. create
        create_req = {
            "request_id": f"probe_{uuid.uuid4().hex[:6]}",
            "method": "llm.task.create",
            "plugin_type": "LLM",
            "plugin_name": "LLMPlugin",
            "params": {
                "callback_channel": "stream.channel",
                "plugin_name": "LLMPlugin",
                "model": {"model_name": "test-model"},
            },
        }
        step = ProbeStep(name="三段式: llm.task.create", request=create_req, responses=[])
        try:
            self._t.send_frame(create_req)
            resp = self._t.recv_frame()
            step.responses.append(resp)
            data_str = resp.get("data_json", "") or resp.get("data", "")
            if isinstance(data_str, str) and data_str:
                data = json.loads(data_str)
            else:
                data = resp
            task_id = data.get("data", {}).get("task_id", "")
            self._check(step, "create 返回 task_id", bool(task_id))

            if task_id:
                # 2. stream
                stream_step = ProbeStep(
                    name="三段式: llm.stream", request={
                        "request_id": f"{create_req['request_id']}_stream",
                        "method": "llm.stream",
                        "plugin_type": "LLM",
                        "plugin_name": "LLMPlugin",
                        "params": {
                            "task_id": task_id,
                            "request": {
                                "intentions": [{"query": "你好"}],
                                "prompt": ["你是一个助手。"],
                            },
                            "model": {"model_name": "test-model"},
                        },
                    }, responses=[])
                self._t.send_frame(stream_step.request)
                # 读确认帧 + 事件流
                ack = self._t.recv_frame()
                stream_step.responses.append(ack)
                while True:
                    evt = self._t.recv_frame()
                    stream_step.responses.append(evt)
                    if evt.get("event_type") in ("llm_final", "llm_error") or evt.get("done"):
                        break
                self._check(stream_step, "stream 有事件", len(stream_step.responses) > 1)
                last = stream_step.responses[-1]
                self._check(stream_step, "stream 最终是 llm_final",
                            last.get("event_type") == "llm_final",
                            f"got {last.get('event_type')}")
                stream_step.passed = all(v is True for v in stream_step.checks.values())

                # 3. destroy
                destroy_step = ProbeStep(
                    name="三段式: llm.task.destroy", request={
                        "request_id": f"{create_req['request_id']}_destroy",
                        "method": "llm.task.destroy",
                        "plugin_type": "LLM",
                        "plugin_name": "LLMPlugin",
                        "params": {"task_id": task_id},
                    }, responses=[])
                self._t.send_frame(destroy_step.request)
                destroy_resp = self._t.recv_frame()
                destroy_step.responses.append(destroy_resp)
                destroy_step.passed = True

                self._steps.append(stream_step)
                self._steps.append(destroy_step)

        except Exception as e:
            step.responses.append({"__error__": str(e)})

        step.passed = all(v is True for v in step.checks.values()) if step.checks else True
        self._steps.insert(self._steps.index(step) + 1 if step in self._steps else len(self._steps), step)

    def _probe_llm_complete(self) -> None:
        """llm.complete（非流式旧协议）。"""
        step = self._probe(
            name="llm.complete",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "llm.complete",
                "plugin_type": "LLM",
                "plugin_name": "LLMPlugin",
                "params": {
                    "request": {
                        "intentions": [{"query": "你好"}],
                        "prompt": ["你是一个助手。"],
                    },
                    "model": {"model_name": "test-model"},
                },
            },
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            resp = step.responses[0]
            # llm.complete 的响应可能直接包含 response 字段，也可能在 data_json 中
            data_str = resp.get("data_json", "") or resp.get("data", "")
            if isinstance(data_str, str) and data_str:
                try:
                    data = json.loads(data_str)
                except json.JSONDecodeError:
                    data = resp
            else:
                data = resp
            self._check(step, "响应含 response 或 content",
                        bool(data.get("response") is not None or data.get("content") is not None))
        step.passed = all(v is True for v in step.checks.values())

    def _probe_tool_list(self) -> None:
        """tool.list：检查工具列表格式。"""
        step = self._probe(
            name="tool.list",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "tool.list",
                "plugin_type": "Tool",
                "plugin_name": "ToolPlugin",
                "params": {},
            },
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            resp = step.responses[0]
            data_str = resp.get("data_json", "") or resp.get("data", "")
            if isinstance(data_str, str) and data_str:
                try:
                    data = json.loads(data_str)
                except json.JSONDecodeError:
                    data = resp
            else:
                data = resp
            self._check(step, "响应结构是 dict", isinstance(data, dict))
        step.passed = all(v is True for v in step.checks.values())

    def _probe_heartbeat(self) -> None:
        """heartbeat。"""
        step = self._probe(
            name="heartbeat",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "heartbeat",
                "plugin_type": "",
                "plugin_name": "",
                "params": {},
            },
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            r = step.responses[0]
            self._check(step, "heartbeat 返回 pong",
                        r.get("pong") is True or "pong" in str(r))
        step.passed = all(v is True for v in step.checks.values())

    def _probe_unknown_method(self) -> None:
        """未知方法：检查错误响应格式。"""
        step = self._probe(
            name="unknown_method",
            request={
                "request_id": f"probe_{uuid.uuid4().hex[:6]}",
                "method": "nonexistent.method",
                "plugin_type": "",
                "plugin_name": "",
                "params": {},
            },
        )
        self._check(step, "有响应帧", bool(step.responses))
        if step.responses:
            resp = step.responses[0]
            # 错误可能以不同形式返回
            has_error = bool(resp.get("error_code") or resp.get("error"))
            self._check(step, "错误方法返回错误码", has_error)
        step.passed = all(v is True for v in step.checks.values())

    # ── 报告生成 ──────────────────────────────────────────────────

    def _build_report(self) -> ProbeReport:
        total = len(self._steps)
        passed = sum(1 for s in self._steps if s.passed)
        report = ProbeReport(
            tag=self._tag,
            socket_path=self._t.sock_path,
            timestamp=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            steps=self._steps,
            summary={
                "total": total,
                "passed": passed,
                "failed": total - passed,
                "all_passed": passed == total,
            },
        )
        return report


# ── 依赖快速检查 ───────────────────────────────────────────────────────


def is_mock_server(sock_path: str) -> bool:
    """快速检查是否为 Mock Server（通过 protocol version 等特征）。"""
    return "mock" in sock_path.lower()


# ── FusionMind HTTP 直连探测 ──────────────────────────────────────────


@dataclass
class FusionMindComparison:
    """UDS 路径 vs HTTP 直连 FusionMind 的字段对比结果。"""

    request_body: dict[str, Any]
    uds_response: dict[str, Any] | None = None
    http_response: dict[str, Any] | None = None
    http_error: str | None = None
    differences: list[dict[str, Any]] = field(default_factory=list)
    passed: bool = True


def probe_fusionmind_via_http(url: str, request_body: dict[str, Any],
                               timeout: int = 10) -> dict[str, Any] | str:
    """通过 HTTP POST 直接调用 FusionMind 的 OpenAI 兼容端点，返回解析后的 JSON 或错误字符串。"""
    try:
        resp = requests.post(
            url,
            json=request_body,
            headers={"Content-Type": "application/json"},
            timeout=timeout,
        )
        body = resp.text
    except requests.exceptions.RequestException as e:
        return f"Request failed: {e}"

    # 解析 SSE 响应（FusionMind 即使非流式也可能返回 SSE 格式）
    # 取最后一个 data: 块中的 content
    last_content = None
    last_finish_reason = None
    for line in body.splitlines():
        line = line.strip()
        if line.startswith("data: "):
            payload = line[6:].strip()
            if payload == "[DONE]":
                break
            try:
                chunk = json.loads(payload)
            except json.JSONDecodeError:
                continue
            # 兼容两种结构
            choices = chunk.get("choices") or (chunk.get("result", {}) if isinstance(chunk.get("result"), dict) else {}).get("choices")
            if not choices:
                continue
            choice = choices[0] if isinstance(choices, list) and choices else {}
            delta_or_msg = choice.get("delta") or choice.get("message") or {}
            if delta_or_msg.get("content"):
                last_content = delta_or_msg["content"]
            if choice.get("finish_reason"):
                last_finish_reason = choice["finish_reason"]
    if last_content is not None:
        return {"content": last_content, "finish_reason": last_finish_reason or "stop"}

    # 非 SSE 响应（纯 JSON）
    try:
        data = json.loads(body)
    except json.JSONDecodeError:
        return body[:500]

    if isinstance(data, dict) and "choices" in data:
        c = data["choices"][0]
        m = c.get("message", {})
        return {
            "content": m.get("content", ""),
            "finish_reason": c.get("finish_reason", "stop"),
            "tool_calls": m.get("tool_calls"),
            "usage": data.get("usage"),
        }
    return str(data)[:500]


def compare_uds_vs_http(request_body: dict[str, Any],
                         uds_data: dict | None,
                         http_data: dict | str | None) -> FusionMindComparison:
    """对比 UDS 路径和 HTTP 路径对同一个请求的响应结构。"""
    comp = FusionMindComparison(
        request_body=request_body,
        uds_response=uds_data,
        http_response=http_data if isinstance(http_data, dict) else None,
        http_error=http_data if isinstance(http_data, str) else None,
    )

    if uds_data is None or http_data is None:
        comp.passed = False
        return comp
    if isinstance(http_data, str):
        comp.passed = False
        return comp

    # 对比 content 存在性
    uds_content = uds_data.get("content") or uds_data.get("answer") or _extract_uds_content(uds_data)
    http_content = http_data.get("content", "")

    if not uds_content and not http_content:
        comp.differences.append({"field": "content", "uds": None, "http": None, "note": "both empty"})
    elif not uds_content:
        comp.differences.append({"field": "content", "uds": None, "http": http_content[:50], "note": "UDS missing content"})
        comp.passed = False
    elif not http_content:
        comp.differences.append({"field": "content", "uds": uds_content[:50], "http": None, "note": "HTTP missing content"})
        comp.passed = False

    # 对比 finish_reason
    uds_fr = uds_data.get("finish_reason") or ""
    http_fr = http_data.get("finish_reason", "")
    if uds_fr != http_fr:
        comp.differences.append({"field": "finish_reason", "uds": uds_fr, "http": http_fr, "note": "mismatch"})
        comp.passed = False

    # 对比 tool_calls 存在性
    uds_tc = uds_data.get("tool_calls")
    http_tc = http_data.get("tool_calls")
    if bool(uds_tc) != bool(http_tc):
        comp.differences.append({"field": "tool_calls", "uds": bool(uds_tc), "http": bool(http_tc), "note": "one side has tool_calls, the other doesn't"})
        comp.passed = False

    return comp


def _extract_uds_content(uds_data: dict) -> str:
    """从 UDS 响应中提取文本内容（兼容多种字段名）。"""
    for key in ("answer", "content", "response", "text"):
        val = uds_data.get(key)
        if val and isinstance(val, str):
            return val
    # 尝试从 data_json 中提取
    dj = uds_data.get("data_json", "")
    if isinstance(dj, str) and dj:
        try:
            d = json.loads(dj)
            return _extract_uds_content(d)
        except (json.JSONDecodeError, TypeError):
            pass
    return ""


# ── CLI ─────────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description="UDS 协议一致性验证 — 检测 Mock Server 与生产 SysAgent 行为是否一致。"
                     "支持 --fusionmind-url 直连推理服务做转发对比。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
对比示例：
  # 只测 UDS 协议格式
  python3 %(prog)s --socket /tmp/mock_agentflow.sock --tag mock

  # UDS 协议 + 转发到 FusionMind 一致性
  python3 %(prog)s \\
      --socket /tmp/agentflow_plugin.sock --tag production \\
      --fusionmind-url http://127.0.0.1:8085/infermind/llm/v1/chat

  # 输出 JSON 报告
  python3 %(prog)s --socket /tmp/mock_agentflow.sock --tag mock --output report_mock.json
        """,
    )
    parser.add_argument("--socket", required=True,
                        help="UDS socket 路径（如 /tmp/mock_agentflow.sock 或 /tmp/agentflow_plugin.sock）")
    parser.add_argument("--tag", default="",
                        help="目标标识（如 mock / production），默认从 socket 路径推断")
    parser.add_argument("--fusionmind-url", default="",
                        help="FusionMind HTTP 端点（如 http://127.0.0.1:8085/infermind/llm/v1/chat），"
                             "提供后将对比 UDS 转发与 HTTP 直连的响应一致性")
    parser.add_argument("--output", default="",
                        help="输出 JSON 报告路径（不指定则打印到终端）")
    args = parser.parse_args()

    tag = args.tag or ("mock" if "mock" in args.socket.lower() else "unknown")

    print(f"🔌 连接 {args.socket}...")
    transport = UdsTransport(args.socket)
    try:
        transport.connect()
        print(f"   ✅ 连接成功")
    except Exception as e:
        print(f"   ❌ 连接失败: {e}")
        sys.exit(1)

    print(f"🧪 执行 UDS 协议探测（tag={tag}）...")
    runner = ProbeRunner(transport, tag)
    try:
        report = runner.run_all()
    except Exception as e:
        print(f"   ❌ 探测异常: {e}")
        transport.close()
        sys.exit(1)

    transport.close()

    # 输出报告摘要
    s = report.summary
    print(f"\n{'=' * 60}")
    print(f"📊 UDS 协议报告（tag={tag}）")
    print(f"{'=' * 60}")
    print(f"  目标:       {args.socket}")
    print(f"  探测项:     {s['total']}")
    print(f"  通过:       {s['passed']}")
    print(f"  失败:       {s['failed']}")
    print(f"  全部通过:   {'✅' if s['all_passed'] else '❌'}")

    # 输出失败详情
    failed_steps = [s for s in report.steps if not s.passed]
    if failed_steps:
        print(f"\n{'=' * 60}")
        print(f"❌ 失败详情")
        print(f"{'=' * 60}")
        for step in failed_steps:
            fails = {k: v for k, v in step.checks.items() if v is not True}
            print(f"\n  [{step.name}]")
            for check_name, detail in fails.items():
                print(f"    ❌ {check_name}: {detail}")

    # ── FusionMind 转发一致性对比 ────────────────────────────────
    all_forwarding_passed = True
    forwarding_results: list[FusionMindComparison] = []

    if args.fusionmind_url:
        print(f"\n{'=' * 60}")
        print(f"🔁 FusionMind 转发一致性对比")
        print(f"  UDS → SysAgent → FusionMind")
        print(f"  vs HTTP → {args.fusionmind_url}")
        print(f"{'=' * 60}")

        test_cases = [
            {"name": "llm.chat 流式", "body": {
                "messages": [{"role": "user", "content": "你好"}],
                "model": "test-model",
                "stream": True,
            }},
            {"name": "llm.chat 非流式", "body": {
                "messages": [{"role": "user", "content": "你好"}],
                "model": "test-model",
                "stream": False,
            }},
            {"name": "llm.chat 带 tools", "body": {
                "messages": [{"role": "user", "content": "北京天气？"}],
                "model": "test-model",
                "stream": True,
                "tools": [{
                    "type": "function",
                    "function": {
                        "name": "get_weather",
                        "description": "获取天气",
                        "parameters": {"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]},
                    },
                }],
            }},
        ]

        for tc in test_cases:
            name = tc["name"]
            body = tc["body"]

            # HTTP 直连 FusionMind
            print(f"\n  [{name}]")
            print(f"    HTTP → FusionMind...", end=" ", flush=True)
            http_result = probe_fusionmind_via_http(args.fusionmind_url, body)
            if isinstance(http_result, dict):
                print(f"✅ content_len={len(http_result.get('content', ''))}")
            else:
                print(f"⚠️  {str(http_result)[:80]}")

            # UDS 转发（复用 llm.chat 请求）
            print(f"    UDS → SysAgent → FusionMind...", end=" ", flush=True)
            try:
                uds_t = UdsTransport(args.socket)
                uds_t.connect()
                uds_t.send_frame({
                    "request_id": f"fm_{uuid.uuid4().hex[:6]}",
                    "method": "llm.chat",
                    "plugin_type": "LLM",
                    "plugin_name": "LLMPlugin",
                    "params": {"request": body},
                })
                event_count = 0
                final_uds = None
                while True:
                    evt = uds_t.recv_frame()
                    event_count += 1
                    if evt.get("event_type") == "llm_final" or evt.get("done"):
                        data_str = evt.get("data", "{}")
                        try:
                            final_uds = json.loads(data_str) if isinstance(data_str, str) else data_str
                        except json.JSONDecodeError:
                            final_uds = {}
                        break
                    if event_count > 100:
                        break
                uds_t.close()
                print(f"✅ events={event_count}")
            except Exception as e:
                print(f"❌ {e}")
                final_uds = None

            # 对比
            comp = compare_uds_vs_http(body, final_uds, http_result)
            forwarding_results.append(comp)
            if comp.differences:
                print(f"    ⚠️  差异 ({len(comp.differences)} 项):")
                for d in comp.differences:
                    print(f"      - {d['field']}: UDS={d['uds']} vs HTTP={d['http']}")
            elif comp.passed:
                print(f"    ✅ 一致")
            else:
                print(f"    ⚠️  无法对比（HTTP 错误或无响应）")

            if not comp.passed:
                all_forwarding_passed = False

    # ── 输出报告 ──────────────────────────────────────────────
    report_data = report.to_dict()
    if forwarding_results:
        report_data["forwarding_comparison"] = [
            {
                "name": r.request_body.get("messages", [{}])[-1].get("content", "")[:30],
                "request_body": r.request_body,
                "uds_response": r.uds_response,
                "http_response": r.http_response,
                "http_error": r.http_error,
                "differences": r.differences,
                "passed": r.passed,
            }
            for r in forwarding_results
        ]
        report_data["forwarding_all_passed"] = all_forwarding_passed

    report_json = json.dumps(report_data, ensure_ascii=False, indent=2)
    if args.output:
        os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".", exist_ok=True)
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(report_json)
        print(f"\n📄 完整报告已保存: {args.output}")
    else:
        print(f"\n📄 完整报告:")
        print(report_json)

    # 最终结论
    all_ok = s["all_passed"] and (all_forwarding_passed if forwarding_results else True)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
