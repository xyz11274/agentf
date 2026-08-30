"""
TDD Red/Green tests for streaming chunk push — OpenAiCompatibleClient.on_chunk.

These tests lock the streaming contract:
  - stream=True + on_chunk set  → callback fires per non-empty delta token
  - stream=False                  → callback never fires
  - on_chunk raising              → main LLM call still completes (resilience)
  - on_chunk payload format       → sanitized SSE JSON with equivalent delta fields

The on_chunk mechanism is the foundation that llm_call.py builds on to wire
streaming chunks to DataBus.response_push (see streaming-response.md §3.5/§3.6).
"""

from __future__ import annotations

import json
from unittest.mock import MagicMock

import pytest
from agentflow_runtime.common.clients import openai_compatible_client
from agentflow_runtime.common.clients.openai_compatible_client import OpenAiCompatibleClient




# ── Fake SSE Response ──


class _FakeSseResponse:
    """Minimal requests.Response stand-in that yields SSE lines via iter_lines."""

    def __init__(self, lines: list[str]):
        self._lines = lines
        self.encoding = "utf-8"
        self.status_code = 200

    def iter_lines(self, decode_unicode: bool = True):
        for line in self._lines:
            yield line


def _sse_line(chunk_dict: dict) -> str:
    """Render a chunk dict as a single SSE ``data:`` line."""
    return "data: " + json.dumps(chunk_dict, ensure_ascii=False)


SSE_LINES = [
    _sse_line({"choices": [{"delta": {"content": "你好"}, "index": 0}]}),
    _sse_line({"choices": [{"delta": {"content": "世界"}, "index": 0}]}),
    _sse_line({
        "choices": [{"delta": {"content": "！"}, "finish_reason": "stop", "index": 0}],
    }),
    "data: [DONE]",
]


@pytest.fixture
def client() -> OpenAiCompatibleClient:
    return OpenAiCompatibleClient(
        model="test-model",
        base_url="http://127.0.0.1:9/v1",
        api_key="test-key",
        request_timeout=5,
        connect_timeout=1,
    )


# ═══════════════════════════════════════════════════════════════════
# Contract: on_chunk callback contract
# ═══════════════════════════════════════════════════════════════════


class TestOnChunkCallback:
    """Verify the on_chunk callback signature/contract."""

    def test_on_chunk_called_per_token(self, client, monkeypatch):
        """stream=True with 3 content deltas → on_chunk fires exactly 3 times,
        each invocation receives a sanitized SSE payload JSON string."""
        fake_resp = _FakeSseResponse(SSE_LINES)
        # requests.post returns the streaming response once, then is never re-entered
        # (stream=True skips retry-loop because max_attempts=1 for stream)
        monkeypatch.setattr(
            openai_compatible_client.requests, "post",
            MagicMock(return_value=fake_resp),
        )

        chunks_received: list[str] = []

        def _on_chunk(raw_json: str) -> None:
            chunks_received.append(raw_json)

        response = client.chat_completion(
            [{"role": "user", "content": "hi"}],
            stream=True,
            on_chunk=_on_chunk,
        )

        assert len(chunks_received) == 3, (
            f"expected 3 chunk callbacks (one per non-empty token), "
            f"got {len(chunks_received)}: {chunks_received}"
        )
        for raw_json in chunks_received:
            parsed = json.loads(raw_json)
            assert "choices" in parsed
            assert parsed["choices"][0]["delta"]["content"], \
                "callback payload must contain the sanitized delta content"
        assert response["choices"][0]["message"]["content"] == "你好世界！"

    def test_on_chunk_not_called_when_stream_false(self, client, monkeypatch):
        """stream=False → on_chunk must NEVER fire (no SSE loop runs)."""
        monkeypatch.setattr(
            openai_compatible_client.requests, "post",
            MagicMock(return_value=MagicMock(json=lambda: {
                "choices": [{"message": {"content": "ok"}, "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            })),
        )

        calls: list[str] = []

        def _on_chunk(raw_json: str) -> None:
            calls.append(raw_json)

        response = client.chat_completion(
            [{"role": "user", "content": "hi"}],
            stream=False,
            on_chunk=_on_chunk,  # would be ignored
        )

        assert calls == [], f"stream=False must not invoke on_chunk, got {calls}"
        assert response["choices"][0]["message"]["content"] == "ok"

    def test_on_chunk_swallows_exceptions(self, client, monkeypatch):
        """on_chunk raising must NOT abort the LLM call (resilience invariant
        from streaming-response.md §3.7 — trace path failures must not interrupt
        the main flow). The final response_data must still be returned."""
        fake_resp = _FakeSseResponse(SSE_LINES)
        monkeypatch.setattr(
            openai_compatible_client.requests, "post",
            MagicMock(return_value=fake_resp),
        )

        call_count = {"n": 0}

        def _bad_callback(raw_json: str) -> None:
            call_count["n"] += 1
            raise RuntimeError(f"synthetic chunk callback failure #{call_count['n']}")

        response = client.chat_completion(
            [{"role": "user", "content": "hi"}],
            stream=True,
            on_chunk=_bad_callback,
        )

        assert call_count["n"] == 3, \
            f"callback must have been tried for all 3 tokens (got {call_count['n']})"
        assert response["choices"][0]["message"]["content"] == "你好世界！", \
            "response_data must be returned even when on_chunk raises"

    def test_on_chunk_none_is_valid_no_op(self, client, monkeypatch):
        """on_chunk=None (the default) must work — no AttributeError, normal flow."""
        fake_resp = _FakeSseResponse(SSE_LINES)
        monkeypatch.setattr(
            openai_compatible_client.requests, "post",
            MagicMock(return_value=fake_resp),
        )

        response = client.chat_completion(
            [{"role": "user", "content": "hi"}],
            stream=True,
            # on_chunk omitted → defaults to None
        )

        assert response["choices"][0]["message"]["content"] == "你好世界！"

    def test_on_chunk_payload_preserves_delta_fields(self, client, monkeypatch):
        """Callback preserves non-secret delta fields after safe re-serialization."""
        specific_payload = (
            '{"choices":[{"delta":{"content":"X","custom_field":42},"index":0}]}'
        )
        fake_resp = _FakeSseResponse([
            f"data: {specific_payload}",
            "data: [DONE]",
        ])
        monkeypatch.setattr(
            openai_compatible_client.requests, "post",
            MagicMock(return_value=fake_resp),
        )

        received: list[str] = []

        def _on_chunk(raw_json: str) -> None:
            received.append(raw_json)

        client.chat_completion(
            [{"role": "user", "content": "hi"}],
            stream=True,
            on_chunk=_on_chunk,
        )

        assert len(received) == 1
        parsed = json.loads(received[0])
        delta = parsed["choices"][0]["delta"]
        assert delta["content"] == "X"
        assert delta["custom_field"] == 42