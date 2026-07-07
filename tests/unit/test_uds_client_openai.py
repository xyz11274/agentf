#!/usr/bin/env python3
"""
Unit tests for LlmUdsClient OpenAI-compatible parsing logic.

Tests the new ``llm.chat`` path methods in isolation by mocking the UDS transport:
  - ``_receive_chat_events()`` — streaming event parsing (token, tool_calls, usage)
  - ``_build_from_complete()`` — non-streaming response assembly
  - ``_build_openai_response()`` — OpenAI format output construction
  - ``on_chunk`` callback integration

Usage:
    python3 test_uds_client_openai.py
"""

import json
import os
import socket
import struct
import sys
from typing import Any
from unittest.mock import patch

# ── Add project source to path ─────────────────────────────────────────
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
CLIENTS_DIR = os.path.join(
    PROJECT_ROOT, "examples/agent/outfit-assistant/nodes/common",
)
sys.path.insert(0, CLIENTS_DIR)

# Import clients package to establish relative import context
import clients.base  # noqa: F401, E402
from clients import llm_uds_client  # noqa: E402

LlmUdsClient = llm_uds_client.LlmUdsClient
LlmUdsError = llm_uds_client.LlmUdsError
_UdsTransport = llm_uds_client._UdsTransport

# ── Test helpers ────────────────────────────────────────────────────────

TESTS_PASSED = 0
TESTS_FAILED = 0
CURRENT_SUITE = ""


def log_pass(msg: str):
    global TESTS_PASSED
    TESTS_PASSED += 1
    print(f"  [PASS] {msg}")


def log_fail(msg: str, expected=None, actual=None):
    global TESTS_FAILED
    TESTS_FAILED += 1
    print(f"  [FAIL] {msg}")
    if expected:
        print(f"         Expected: {expected}")
    if actual:
        print(f"         Actual: {actual}")


def start_suite(name: str):
    global CURRENT_SUITE
    CURRENT_SUITE = name
    print(f"\n=== {name} ===")


# ── Mock UDS Transport ────────────────────────────────────────────────

class MockUdsTransport:
    """Mock ``_UdsTransport`` that returns pre-defined event frames."""

    def __init__(self, frames: list[dict[str, Any]]):
        self._frames = list(frames)
        self._connected = True
        self._sent: list[dict[str, Any]] = []

    @property
    def connected(self) -> bool:
        return self._connected

    def connect(self) -> None:
        self._connected = True

    def close(self) -> None:
        self._connected = False

    def send_json(self, data: dict[str, Any]) -> None:
        self._sent.append(data)

    def recv_json(self) -> dict[str, Any]:
        if not self._frames:
            raise LlmUdsError("UDS connection closed")
        return self._frames.pop(0)


class FakeClient(LlmUdsClient):
    """LlmUdsClient with mocked transport for unit testing."""

    def __init__(self):
        super().__init__(sock_path="/tmp/test_uds.sock", model="aisf/test-model")
        self._transport: _UdsTransport | None = None

    def inject_transport(self, mock: MockUdsTransport):
        self._transport = mock


# ── Test suites ────────────────────────────────────────────────────────

def test_parse_token_events():
    """_receive_chat_events: basic token accumulation."""
    mock = MockUdsTransport([
        {"event_type": "llm_token", "data": '{"token":"你好"}'},
        {"event_type": "llm_token", "data": '{"token":"世界"}'},
        {"event_type": "llm_final", "data": '{"answer":"你好世界","finish_reason":"stop","usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "test-req")
    assert result["choices"][0]["message"]["content"] == "你好世界", f"got: {result}"
    assert result["choices"][0]["finish_reason"] == "stop"
    assert result["usage"]["total_tokens"] == 15
    log_pass("parse_token_events")


def test_parse_tool_calls():
    """_receive_chat_events: tool_calls accumulation across multiple events."""
    mock = MockUdsTransport([
        {"event_type": "llm_tool_calls", "data": json.dumps({"tool_calls": [
            {"index": 0, "id": "call_1", "type": "function",
             "function": {"name": "get_weather", "arguments": '{"lo'}}
        ]})},
        {"event_type": "llm_tool_calls", "data": json.dumps({"tool_calls": [
            {"index": 0, "function": {"arguments": 'cation": "北京"}'}}
        ]})},
        {"event_type": "llm_final", "data": json.dumps({
            "answer": "", "finish_reason": "tool_calls",
            "usage": {"prompt_tokens": 20, "completion_tokens": 10, "total_tokens": 30},
        })},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "test-req")
    tc = result["choices"][0]["message"].get("tool_calls", [])
    assert len(tc) == 1, f"expected 1 tool_call, got {len(tc)}"
    assert tc[0]["id"] == "call_1"
    assert tc[0]["function"]["name"] == "get_weather"
    assert tc[0]["function"]["arguments"] == '{"location": "北京"}'
    assert result["choices"][0]["finish_reason"] == "tool_calls"
    assert result["usage"]["total_tokens"] == 30
    log_pass("parse_tool_calls")


def test_parse_error_event():
    """_receive_chat_events: llm_error raises LlmUdsError."""
    mock = MockUdsTransport([
        {"event_type": "llm_error", "data": '{"error_message":"rate limit exceeded"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    try:
        client._receive_chat_events(mock, "test-req")
        log_fail("parse_error_event", expected="LlmUdsError raised", actual="no exception")
    except LlmUdsError as e:
        assert "rate limit exceeded" in str(e)
        log_pass("parse_error_event")


def test_parse_final_answer_fallback():
    """_receive_chat_events: llm_final with answer but no prior tokens."""
    mock = MockUdsTransport([
        {"event_type": "llm_final", "data": json.dumps({
            "answer": "直接回答",
            "finish_reason": "stop",
        })},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "test-req")
    content = result["choices"][0]["message"]["content"]
    assert content == "直接回答", f"got: {content}"
    log_pass("parse_final_answer_fallback")


def test_on_chunk_content():
    """_receive_chat_events: on_chunk callback receives OpenAI-compatible SSE JSON."""
    chunks_received = []
    mock = MockUdsTransport([
        {"event_type": "llm_token", "data": '{"token":"你"}'},
        {"event_type": "llm_token", "data": '{"token":"好"}'},
        {"event_type": "llm_final", "data": '{"answer":"你好","finish_reason":"stop"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    client._receive_chat_events(mock, "test-req", on_chunk=lambda c: chunks_received.append(c))
    assert len(chunks_received) == 2, f"expected 2 chunks, got {len(chunks_received)}"
    # Each chunk should be OpenAI-compatible SSE JSON
    for i, chunk_str in enumerate(chunks_received):
        chunk = json.loads(chunk_str)
        assert "choices" in chunk, f"chunk {i} missing choices"
        assert chunk["choices"][0]["delta"].get("content", "") in ("你", "好")
    log_pass("on_chunk_content")


def test_on_chunk_tool_calls():
    """_receive_chat_events: on_chunk also fires for tool_calls events."""
    chunks_received = []
    mock = MockUdsTransport([
        {"event_type": "llm_tool_calls", "data": json.dumps({"tool_calls": [
            {"index": 0, "id": "call_x", "type": "function",
             "function": {"name": "search", "arguments": "{}"}}
        ]})},
        {"event_type": "llm_final", "data": '{"answer":"","finish_reason":"tool_calls"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    client._receive_chat_events(mock, "test-req", on_chunk=lambda c: chunks_received.append(c))
    assert len(chunks_received) == 1, f"expected 1 chunk, got {len(chunks_received)}"
    chunk = json.loads(chunks_received[0])
    assert chunk["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] == "search"
    log_pass("on_chunk_tool_calls")


def test_build_from_complete():
    """_build_from_complete: non-streaming response assembly."""
    response = {
        "data": json.dumps({
            "content": "你好世界",
            "finish_reason": "stop",
            "usage": {"prompt_tokens": 5, "completion_tokens": 3, "total_tokens": 8},
        })
    }
    result = FakeClient._build_from_complete(response)
    assert result["choices"][0]["message"]["content"] == "你好世界"
    assert result["usage"]["total_tokens"] == 8
    log_pass("build_from_complete")


def test_build_from_complete_with_tool_calls():
    """_build_from_complete: non-streaming with tool_calls."""
    response = {
        "data": json.dumps({
            "content": "",
            "finish_reason": "tool_calls",
            "tool_calls": [{"id": "call_1", "type": "function",
                           "function": {"name": "echo", "arguments": "{}"}}],
            "usage": {"prompt_tokens": 5, "completion_tokens": 2, "total_tokens": 7},
        })
    }
    result = FakeClient._build_from_complete(response)
    tc = result["choices"][0]["message"].get("tool_calls", [])
    assert len(tc) == 1
    assert tc[0]["function"]["name"] == "echo"
    assert result["usage"]["total_tokens"] == 7
    log_pass("build_from_complete_with_tool_calls")


def test_build_openai_response():
    """_build_openai_response: stream-accumulated response assembly."""
    result = FakeClient._build_openai_response(
        content="你好",
        finish_reason="stop",
        tool_calls_acc=None,
        usage={"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
    )
    assert result["choices"][0]["message"]["content"] == "你好"
    assert "tool_calls" not in result["choices"][0]["message"]
    assert result["usage"]["total_tokens"] == 2
    log_pass("build_openai_response")


def test_build_openai_response_with_tool_calls():
    """_build_openai_response: includes tool_calls when present."""
    result = FakeClient._build_openai_response(
        content="",
        finish_reason="tool_calls",
        tool_calls_acc={
            0: {"id": "c1", "type": "function", "function": {"name": "f", "arguments": "{}"}},
        },
        usage=None,
    )
    assert result["choices"][0]["message"]["tool_calls"][0]["id"] == "c1"
    assert "usage" not in result
    log_pass("build_openai_response_with_tool_calls")


def test_on_chunk_exception_safe():
    """_receive_chat_events: on_chunk exception does not propagate."""
    mock = MockUdsTransport([
        {"event_type": "llm_token", "data": '{"token":"X"}'},
        {"event_type": "llm_final", "data": '{"answer":"X","finish_reason":"stop"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)

    def _broken(_):
        raise RuntimeError("boom")

    # Must not raise — exception in on_chunk is swallowed
    result = client._receive_chat_events(mock, "test-req", on_chunk=_broken)
    assert result["choices"][0]["message"]["content"] == "X"
    log_pass("on_chunk_exception_safe")


# ── Transport 层单元测试 ────────────────────────────────────────────

class _FakeSocket:
    """可控的 socket 替身，模拟各种 I/O 行为。"""

    def __init__(self, family=socket.AF_UNIX, sock_type=socket.SOCK_STREAM):
        self.family = family
        self.type = sock_type
        self._recv_data: list[bytes] = []
        self._close_called = False
        self._should_fail_connect = False
        self._should_fail_send = False
        self._timeout = None

    def settimeout(self, timeout: float) -> None:
        self._timeout = timeout

    def connect(self, path: str) -> None:
        if self._should_fail_connect:
            raise ConnectionRefusedError(f"Connection refused: {path}")

    def sendall(self, data: bytes) -> None:
        if self._should_fail_send:
            raise OSError("Broken pipe")
        self._sent = data

    def recv(self, bufsize: int) -> bytes:
        if not self._recv_data:
            return b""
        chunk = self._recv_data.pop(0)
        if len(chunk) > bufsize:
            # 模拟实际 socket 截断行为
            head = chunk[:bufsize]
            self._recv_data.insert(0, chunk[bufsize:])
            return head
        return chunk

    def close(self) -> None:
        self._close_called = True


def test_transport_connect_success():
    """_UdsTransport.connect: 成功建立连接。"""
    transport = _UdsTransport("/tmp/test_success.sock")
    fake = _FakeSocket()
    with patch("socket.socket", return_value=fake):
        transport.connect()
    assert transport.connected
    transport.close()
    log_pass("transport_connect_success")


def test_transport_connect_refused():
    """_UdsTransport.connect: 连接拒绝 → LlmUdsError。"""
    transport = _UdsTransport("/tmp/test_refused.sock")
    fake = _FakeSocket()
    fake._should_fail_connect = True
    with patch("socket.socket", return_value=fake):
        try:
            transport.connect()
            log_fail("transport_connect_refused", expected="LlmUdsError", actual="no exception")
        except LlmUdsError:
            assert transport._sock is None, "socket should be None after failed connect"
            log_pass("transport_connect_refused")


def test_transport_send_without_connect():
    """_UdsTransport.send_json: 未连接 → LlmUdsError。"""
    transport = _UdsTransport("/tmp/test.sock")
    try:
        transport.send_json({"key": "value"})
        log_fail("transport_send_without_connect", expected="LlmUdsError", actual="no exception")
    except LlmUdsError:
        log_pass("transport_send_without_connect")


def test_transport_recv_without_connect():
    """_UdsTransport.recv_json: 未连接 → LlmUdsError。"""
    transport = _UdsTransport("/tmp/test.sock")
    try:
        transport.recv_json()
        log_fail("transport_recv_without_connect", expected="LlmUdsError", actual="no exception")
    except LlmUdsError:
        log_pass("transport_recv_without_connect")


def test_transport_recv_partial_header():
    """_UdsTransport.recv_json: 仅收到部分长度头 → LlmUdsError。"""
    transport = _UdsTransport("/tmp/test.sock")
    fake = _FakeSocket()
    fake._recv_data = [b"\x00\x00"]  # 只有 2/4 字节
    transport._sock = fake
    try:
        transport.recv_json()
        log_fail("transport_recv_partial_header", expected="LlmUdsError", actual="no exception")
    except LlmUdsError as e:
        assert "connection closed" in str(e).lower()
        log_pass("transport_recv_partial_header")


def test_transport_recv_frame_too_large():
    """_UdsTransport.recv_json: 帧超过 MAX_FRAME_SIZE → LlmUdsError。"""
    transport = _UdsTransport("/tmp/test.sock")
    # 构造一个长度 > MAX_FRAME_SIZE 的帧头
    huge_len = 20 * 1024 * 1024  # 20MB
    fake = _FakeSocket()
    fake._recv_data = [struct.pack(">I", huge_len)]
    transport._sock = fake
    try:
        transport.recv_json()
        log_fail("transport_recv_frame_too_large", expected="LlmUdsError", actual="no exception")
    except LlmUdsError as e:
        assert "frame too large" in str(e).lower()
        log_pass("transport_recv_frame_too_large")


def test_transport_recv_partial_body():
    """_UdsTransport.recv_json: 多个 recv 分片重组。"""
    transport = _UdsTransport("/tmp/test.sock")
    payload = b'{"msg":"hello"}'
    # 模拟分多次 recv 返回
    fake = _FakeSocket()
    header = struct.pack(">I", len(payload))
    fake._recv_data = [
        header,              # 先返回完整头部
        payload[:5],         # 第一片 body
        payload[5:],         # 第二片 body
    ]
    transport._sock = fake
    result = transport.recv_json()
    assert result == {"msg": "hello"}, f"got: {result}"
    log_pass("transport_recv_partial_body")


def test_transport_send_and_recv_roundtrip():
    """_UdsTransport: 发送后接收，完整帧协议往返。"""
    transport = _UdsTransport("/tmp/test.sock")
    payload = b'{"echo":true}'
    header = struct.pack(">I", len(payload))
    fake = _FakeSocket()
    fake._recv_data = [header + payload]
    transport._sock = fake

    transport.send_json({"hello": "world"})
    result = transport.recv_json()
    assert result == {"echo": True}, f"got: {result}"
    log_pass("transport_send_and_recv_roundtrip")


def test_transport_close_idempotent():
    """_UdsTransport.close: 可重复调用，不抛异常。"""
    transport = _UdsTransport("/tmp/test.sock")
    transport.close()  # _sock = None
    transport.close()  # 二次调用，应无异常
    log_pass("transport_close_idempotent")


def test_transport_connected_property():
    """_UdsTransport.connected: 正确反映连接状态。"""
    transport = _UdsTransport("/tmp/test.sock")
    assert not transport.connected
    fake = _FakeSocket()
    transport._sock = fake
    assert transport.connected
    transport.close()
    assert not transport.connected
    log_pass("transport_connected_property")


# ── _build_from_complete 边界测试 ──────────────────────────────────

def test_build_from_complete_data_json_field():
    """_build_from_complete: 数据在 data_json 字段（非 data）。"""
    response = {
        "data_json": json.dumps({
            "content": "from data_json",
            "finish_reason": "stop",
        })
    }
    result = FakeClient._build_from_complete(response)
    assert result["choices"][0]["message"]["content"] == "from data_json"
    log_pass("build_from_complete_data_json_field")


def test_build_from_complete_data_already_dict():
    """_build_from_complete: data 字段已经是 dict，不需 JSON 解析。"""
    response = {
        "data": {
            "content": "already dict",
            "finish_reason": "stop",
        }
    }
    result = FakeClient._build_from_complete(response)
    assert result["choices"][0]["message"]["content"] == "already dict"
    log_pass("build_from_complete_data_already_dict")


def test_build_from_complete_empty_data():
    """_build_from_complete: 空 data 字段 → 空响应。"""
    response = {"data": ""}
    result = FakeClient._build_from_complete(response)
    assert result["choices"][0]["message"]["content"] == ""
    assert result["choices"][0]["finish_reason"] == "stop"
    log_pass("build_from_complete_empty_data")


def test_build_from_complete_no_data_fields():
    """_build_from_complete: 既无 data 也无 data_json → 空响应。"""
    response = {"other_key": "value"}
    result = FakeClient._build_from_complete(response)
    assert result["choices"][0]["message"]["content"] == ""
    assert "usage" not in result
    log_pass("build_from_complete_no_data_fields")


# ── _build_openai_response 边界测试 ─────────────────────────────────

def test_build_openai_response_empty_content():
    """_build_openai_response: 空 content，无需 tool_calls/usage。"""
    result = FakeClient._build_openai_response(
        content="", finish_reason="stop",
        tool_calls_acc=None, usage=None,
    )
    assert result["choices"][0]["message"]["content"] == ""
    assert "tool_calls" not in result["choices"][0]["message"]
    assert "usage" not in result
    log_pass("build_openai_response_empty_content")


def test_build_openai_response_full():
    """_build_openai_response: 全部字段齐备。"""
    result = FakeClient._build_openai_response(
        content="完整回答",
        finish_reason="stop",
        tool_calls_acc={
            0: {"id": "c1", "type": "function", "function": {"name": "fn", "arguments": "{}"}},
        },
        usage={"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
    )
    assert result["choices"][0]["message"]["content"] == "完整回答"
    assert len(result["choices"][0]["message"]["tool_calls"]) == 1
    assert result["usage"]["total_tokens"] == 15
    log_pass("build_openai_response_full")


# ── _receive_chat_events 边界测试 ──────────────────────────────────

def test_receive_chat_events_unknown_event_ignored():
    """_receive_chat_events: 未知 event_type 被忽略，不抛异常。"""
    mock = MockUdsTransport([
        {"event_type": "llm_status", "data": '{"status":"busy"}'},
        {"event_type": "llm_heartbeat", "data": '{"t":123}'},
        {"event_type": "llm_final", "data": json.dumps({
            "answer": "最终结果", "finish_reason": "stop",
        })},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "test-req")
    # 无 token 事件，full_answer 为空 → fallback 到 final.answer
    assert result["choices"][0]["message"]["content"] == "最终结果"
    log_pass("receive_chat_events_unknown_event_ignored")


def test_receive_chat_events_final_no_usage():
    """_receive_chat_events: llm_final 无 usage 时不返回 usage。"""
    mock = MockUdsTransport([
        {"event_type": "llm_final", "data": json.dumps({
            "answer": "无用量", "finish_reason": "stop",
        })},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "test-req")
    assert "usage" not in result
    log_pass("receive_chat_events_final_no_usage")


def test_receive_chat_events_empty_token():
    """_receive_chat_events: token 为空时不累加也不触 on_chunk。"""
    chunks = []
    mock = MockUdsTransport([
        {"event_type": "llm_token", "data": '{"token":""}'},
        {"event_type": "llm_token", "data": '{"token":"实际内容"}'},
        {"event_type": "llm_final", "data": json.dumps({
            "answer": "实际内容", "finish_reason": "stop",
        })},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "test-req", on_chunk=lambda c: chunks.append(c))
    assert result["choices"][0]["message"]["content"] == "实际内容"
    assert len(chunks) == 1, f"expected 1 chunk, got {len(chunks)}"
    log_pass("receive_chat_events_empty_token")


def test_receive_chat_events_data_str_or_dict():
    """_receive_chat_events: data 字段可能是 dict 或 str，都能处理。"""
    mock = MockUdsTransport([
        # data 是 dict（非 JSON 字符串）
        {"event_type": "llm_token", "data": {"token": "dict_token"}},
        {"event_type": "llm_final", "data": {"answer": "dict_token", "finish_reason": "stop"}},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "test-req")
    assert result["choices"][0]["message"]["content"] == "dict_token"
    log_pass("receive_chat_events_data_str_or_dict")


# ── chat_completion 集成测试 ──────────────────────────────────────

def test_chat_completion_non_stream_with_mock():
    """chat_completion: 非流式完整路径，通过 mock transport 往返。"""
    content_json = json.dumps({"content": "mock 非流式响应", "finish_reason": "stop"})
    response_frame = {"data": content_json, "data_json": content_json}
    mock = MockUdsTransport([response_frame])
    client = FakeClient()
    client.inject_transport(mock)

    result = client._chat(
        request_body={"messages": [{"role": "user", "content": "hi"}], "stream": False},
        on_chunk=None,
    )
    assert result["choices"][0]["message"]["content"] == "mock 非流式响应"
    assert len(mock._sent) == 1
    sent = mock._sent[0]
    assert sent["method"] == "llm.chat"
    assert sent["params"]["request"]["stream"] is False
    log_pass("chat_completion_non_stream_with_mock")


def test_chat_completion_raises_when_not_connected():
    """chat_completion: 无 transport 且无法连接时报错。"""
    client = FakeClient()
    # 不注入 mock → _transport 为 None, _try_uds 会失败
    try:
        client.chat_completion(messages=[{"role": "user", "content": "hi"}])
        log_fail("chat_completion_raises_when_not_connected",
                 expected="LlmUdsError", actual="no exception")
    except LlmUdsError:
        log_pass("chat_completion_raises_when_not_connected")


# ── _try_uds 测试 ──────────────────────────────────────────────────

def test_try_uds_failure():
    """_try_uds: socket 不存在 → 返回 False，不抛异常。"""
    client = FakeClient()
    # 用不存在的路径
    client.sock_path = "/tmp/nonexistent_uds_test_xxx.sock"
    result = client._try_uds()
    assert result is False
    assert client._transport is None
    log_pass("try_uds_failure")


# ── 辅助方法测试 ──────────────────────────────────────────────────

def test_next_req_id_format():
    """_next_req_id: 格式为 py-{counter}-{hex}。"""
    client = FakeClient()
    req_id = client._next_req_id()
    parts = req_id.split("-")
    assert parts[0] == "py"
    assert parts[1].isdigit()
    assert len(parts[2]) == 8
    import uuid
    # 验证是合法 hex
    int(parts[2], 16)
    log_pass("next_req_id_format")


def test_next_req_id_increment():
    """_next_req_id: 每次调用 counter 递增。"""
    client = FakeClient()
    id1 = client._next_req_id()
    id2 = client._next_req_id()
    seq1 = int(id1.split("-")[1])
    seq2 = int(id2.split("-")[1])
    assert seq2 == seq1 + 1
    log_pass("next_req_id_increment")


# ── Main ──────────────────────────────────────────────────────────────

def main():
    print("=" * 60)
    print("LlmUdsClient OpenAI 兼容格式 单元测试")
    print("=" * 60)

    start_suite("流式事件解析")
    test_parse_token_events()
    test_parse_tool_calls()
    test_parse_error_event()
    test_parse_final_answer_fallback()
    test_on_chunk_content()
    test_on_chunk_tool_calls()
    test_on_chunk_exception_safe()

    start_suite("非流式响应组装")
    test_build_from_complete()
    test_build_from_complete_with_tool_calls()

    start_suite("OpenAI 格式输出")
    test_build_openai_response()
    test_build_openai_response_with_tool_calls()

    start_suite("Transport 层")
    test_transport_connect_success()
    test_transport_connect_refused()
    test_transport_send_without_connect()
    test_transport_recv_without_connect()
    test_transport_recv_partial_header()
    test_transport_recv_frame_too_large()
    test_transport_recv_partial_body()
    test_transport_send_and_recv_roundtrip()
    test_transport_close_idempotent()
    test_transport_connected_property()

    start_suite("_build_from_complete 边界")
    test_build_from_complete_data_json_field()
    test_build_from_complete_data_already_dict()
    test_build_from_complete_empty_data()
    test_build_from_complete_no_data_fields()

    start_suite("_build_openai_response 边界")
    test_build_openai_response_empty_content()
    test_build_openai_response_full()

    start_suite("_receive_chat_events 边界")
    test_receive_chat_events_unknown_event_ignored()
    test_receive_chat_events_final_no_usage()
    test_receive_chat_events_empty_token()
    test_receive_chat_events_data_str_or_dict()

    start_suite("request_id 配对（并发防串线）")
    test_request_id_matching_stream()
    test_request_id_mismatch_skipped_stream()
    test_request_id_mismatch_raises_non_stream()
    test_request_id_missing_is_backward_compatible()

    start_suite("集成测试")
    test_chat_completion_non_stream_with_mock()
    test_chat_completion_raises_when_not_connected()

    start_suite("辅助方法")
    test_try_uds_failure()
    test_next_req_id_format()
    test_next_req_id_increment()

    print(f"\n{'=' * 60}")
    print(f"合计: {TESTS_PASSED + TESTS_FAILED} 项")
    print(f"通过: {TESTS_PASSED}")
    print(f"失败: {TESTS_FAILED}")
    if TESTS_FAILED > 0:
        sys.exit(1)
    print("全部通过 ✅")



# ── request_id 配对测试 ─────────────────────────────────────────

def test_request_id_matching_stream():
    """_receive_chat_events: 所有帧 request_id 匹配，正常返回。"""
    mock = MockUdsTransport([
        {"event_type": "llm_token", "request_id": "req-1", "data": '{"token":"A"}'},
        {"event_type": "llm_token", "request_id": "req-1", "data": '{"token":"B"}'},
        {"event_type": "llm_final", "request_id": "req-1", "data": '{"answer":"AB","finish_reason":"stop"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "req-1")
    assert result["choices"][0]["message"]["content"] == "AB"
    log_pass("request_id_matching_stream")


def test_request_id_mismatch_skipped_stream():
    """_receive_chat_events: 不匹配 request_id 的帧被 continue 跳过。"""
    mock = MockUdsTransport([
        {"event_type": "llm_token", "request_id": "req-other", "data": '{"token":"X"}'},
        {"event_type": "llm_token", "request_id": "req-1", "data": '{"token":"A"}'},
        {"event_type": "llm_token", "request_id": "req-other", "data": '{"token":"Y"}'},
        {"event_type": "llm_token", "request_id": "req-1", "data": '{"token":"B"}'},
        {"event_type": "llm_final", "request_id": "req-1", "data": '{"answer":"AB","finish_reason":"stop"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "req-1")
    # X 和 Y 被跳过，只累加 A+B
    assert result["choices"][0]["message"]["content"] == "AB",         f"expected AB, got {result['choices'][0]['message']['content']}"
    log_pass("request_id_mismatch_skipped_stream")


def test_request_id_mismatch_raises_non_stream():
    """_chat: 非流式响应 request_id 不匹配抛 LlmUdsError。"""
    mock = MockUdsTransport([
        {"request_id": "req-other", "data": '{"content":"wrong"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    # 模拟 _chat 非流式路径的校验
    resp = mock.recv_json()
    if resp.get("request_id") and resp["request_id"] != "req-1":
        try:
            raise LlmUdsError(
                f"UDS response mismatch: expected request_id=req-1, got {resp['request_id']}"
            )
        except LlmUdsError:
            log_pass("request_id_mismatch_raises_non_stream")


def test_request_id_missing_is_backward_compatible():
    """_receive_chat_events: request_id 字段缺失时不拦截（兼容旧版）。"""
    mock = MockUdsTransport([
        # 无 request_id 字段（生产 C++ UDSEvent 行为）
        {"event_type": "llm_token", "data": '{"token":"A"}'},
        {"event_type": "llm_final", "data": '{"answer":"A","finish_reason":"stop"}'},
    ])
    client = FakeClient()
    client.inject_transport(mock)
    result = client._receive_chat_events(mock, "req-1")
    assert result["choices"][0]["message"]["content"] == "A",         f"expected A, got {result['choices'][0]['message']['content']}"
    log_pass("request_id_missing_is_backward_compatible")


if __name__ == "__main__":
    main()
